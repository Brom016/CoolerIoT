# Wokwi Simulation — Smart Cold Chain Box

Step-by-step guide to run, use, and understand the ESP32 simulation of the cold chain box.
Files in this folder:

| File | Purpose |
|---|---|
| `diagram.json` | The circuit (ESP32 + sensors + OLED + buttons + LEDs) |
| `sketch.ino` | The firmware: control logic, alarms, PCM budget, hash-chained log, **MQTT telemetry + remote commands** |
| `dashboard.html` | Web dashboard — live temperatures, chart, state, remote control (open in browser, no server needed) |
| `libraries.txt` | Library list (for Wokwi VS Code extension / CLI users) |

> **What is simulated:** the *electronics and software logic* of the design.
> **What is not simulated:** the compressor and PCM hardware. YOU play the physics by dragging the temperature sliders — the controller reacts exactly as the real firmware would. The PCM "cold battery" budget IS computed in software. BMP390, MPU-6050 and the GNSS hardware are not simulated either — GNSS is a software route (`GPS_SIM`), and the lid reed switch stands in as the blue button.

---

## STEP 0 — Open Wokwi

**Option A (easiest, browser only):**
1. Go to <https://wokwi.com>
2. Click **New Project → ESP32** (an empty project opens with `sketch.ino`).

**Option B (VS Code):** install the *Wokwi for VS Code* extension, put this folder's files in a workspace, press **F1 → Wokwi: Start Simulator**. (`libraries.txt` is picked up automatically here.)

---

## STEP 1 — Load the circuit

1. In the Wokwi editor, open the **diagram.json** tab (right side, next to `sketch.ino`).
2. Select everything, delete it, and paste the full contents of this project's `diagram.json`.
3. The board and parts appear instantly: ESP32, 3× DS18B20, DHT22, OLED, buzzer, 3 LEDs, 3 buttons, slide switch, potentiometer.

## STEP 2 — Load the firmware

1. Open the **sketch.ino** tab, delete its contents, paste this project's `sketch.ino`.

## STEP 3 — Install libraries (browser version only)

1. Open the **Library Manager** (arrow icon in the left sidebar / "Manage Libraries").
2. Add these seven, one by one:
   - `OneWire`
   - `DallasTemperature`
   - `DHT sensor library`
   - `Adafruit Unified Sensor`
   - `Adafruit SSD1306`
   - `Adafruit GFX Library`
   - `PubSubClient`  ← for MQTT

## STEP 4 — Run

1. Click the green **play button**.
2. Open the **Serial Monitor** (hover over the ESP32 → serial monitor icon). Set baud **115200**.
3. You should see a startup legend explaining every field, then records like:
   ```
   REC #1 | t=00:00 | T1=4.5C T2=4.8C | AMB=40.0C | STATE=OK | BAT=12.4V | PCM=100% HOLD=24.0h | EVENT: DISPATCH
     hash chain: 1A2B3C4D  <- first record, seeded from constant 811C9DC5
     >> Shipment handed to the courier. Monitoring and the custody chain start here.
   REC #2 | t=00:02 | T1=4.5C T2=4.8C | AMB=40.0C | STATE=OK | BAT=12.4V | PCM=100% HOLD=24.0h
     hash chain: 5E6F7A8B  <- links to record #1 (1A2B3C4D)
   ```
   and the OLED shows `COLD-CHAIN OK`, temps ~4.5/4.8 °C, `CMP OFF`, `PCM 100%`.

---

## STEP 5 — Know the controls

| Wokwi part | Interactive control | Simulates |
|---|---|---|
| DS18B20 ×2 | hover → drag the temperature slider | payload temperature (probe 1 top, probe 2 bottom) |
| DHT22 | hover → drag temp/humidity sliders | ambient air — set to **38–42 °C** |
| Slide switch | click | **mains power**: toward the red (3V3) wire = power OK; toward the black (GND) wire = power lost |
| Potentiometer | drag the knob | **battery voltage** 9.0–12.8 V |
| Blue button | hold | **lid open** (door reed switch) |
| Yellow button | press | alarm **mute/acknowledge** |
| Green button | press | **custody handover** event + receipt hash |

LEDs: **green** = compressor running, **red** = excursion/quarantine alarm, **yellow** = battery low.

---

## STEP 6 — Run the 8 scenarios (pass criteria included)

**S1 — Normal operation.** Do nothing for ~30 s. ✔ OLED stays `OK`, PCM ~100%, hash chain grows (`#` increases), no alarms.

**S2 — Compressor hysteresis.** Drag DS18B20 probe 1 to 9 °C. ✔ Green LED turns ON (t > 7 °C). Drag it back to 4 °C → LED turns OFF only below 5 °C (watch it stay ON at 6 °C — that is hysteresis, exactly like a real thermostat).

**S3 — Power loss → passive holdover.** Flip the slide switch to the GND side. ✔ OLED shows `PASSIVE-HOLD`, compressor LED forced OFF, PCM % starts draining (faster at higher ambient/ΔT), `HOLD` shows estimated story-hours left. Switch back → compressor resumes.

**S4 — Door open.** Hold the blue button ~3 s. ✔ Immediate `EVENT: DOOR OPEN` record with a plain-English note in the serial log, OLED `DOOR-OPEN`, PCM drain ×1.3 while held, `EVENT: DOOR CLOSED` on release.

**S5 — Excursion → quarantine (the 15-minute rule).** Set ambient (DHT22) to 42 °C, drag probe 1 to 10 °C and leave it. ✔ `EXC` counter climbs (~1 story-min per real second). Red LED + beeping start immediately. At **15 story-minutes (= 15 real seconds)** the OLED shows `QUARANTINE`, the serial log prints `EVENT: QUARANTINE FLAGGED` with a plain-English note about the QA disposition. The latch stays until restart (safety by design).

**S6 — Battery low.** Drag the potentiometer until BAT shows < 11.0 V. ✔ Yellow LED ON, `EVENT: BATTERY LOW` record. Below **10.5 V** the compressor is shed automatically (green LED off even if warm) — power goes to logging and sensors first.

**S7 — Custody handover.** Press the green button. ✔ Serial prints an `EVENT: CUSTODY HANDOVER` record plus `RECEIPT: chain head <hash> after record #N — share this hash with the receiver.` The receiver would recompute the chain and compare — the "signed receipt" of the real design.

**S8 — Tamper test (data integrity).** Look at consecutive records: each `hash chain:` line shows this record's fingerprint and which previous record it links to. Imagine someone edits an older `REC` line's temperature from 10.0 to 7.9. The fingerprint is FNV-1a of **the REC line text plus the previous fingerprint** — so the edited line's fingerprint changes, and *every link after it* stops matching. To hide the change, the attacker would have to recompute the entire chain — and in the real device the chain head is signed (Ed25519) and anchored, so this becomes infeasible. This is the core trust argument of the essay, demonstrated live.

---

## STEP 7 — Wiring reference (what `diagram.json` contains)

Grouped; colors are suggestions Wokwi renders on screen.

**Power rails**
- `3V3` → DS18B20 ×2 VCC, DHT22 VCC, OLED VCC, pot VCC, slide-switch pin 3
- `GND` → all module GNDs, buzzer pin 1, LED resistors, buttons, switch pin 1

**Temperature (OneWire bus, shared)**
- DS18B20-1/2/3 **DQ** → **GPIO 4** (+ 4.7 kΩ resistor between GPIO 4 and 3V3)
  - Wokwi names the probe data pin `DQ`, and each probe needs a unique `deviceID` attribute (111111111111 / 222222222222 / 333333333333) — see docs.wokwi.com/parts/wokwi-ds18b20
  - probe names follow detail.md §10: **T1 top-centre · T2 bottom-centre · T3 mid wall**

**Ambient**
- DHT22 SDA → **GPIO 5**

**Display (I2C)**
- OLED SDA → **GPIO 21**, SCL → **GPIO 22**

**Outputs**
- Buzzer + → **GPIO 25**
- Green LED anode → **GPIO 26** (cathode → 220 Ω → GND)
- Red LED anode → **GPIO 27** (cathode → 220 Ω → GND)
- Yellow LED anode → **GPIO 14** (cathode → 220 Ω → GND)

**Inputs**
- DOOR button → **GPIO 32** ↔ GND (internal pullup)
- MUTE button → **GPIO 33** ↔ GND
- HANDOVER button → **GPIO 13** ↔ GND
- Slide switch common → **GPIO 23** (pin 1 → GND, pin 3 → 3V3)
- Pot wiper → **GPIO 34** (ADC)

---

## STEP 8 — From simulation to real hardware

| Wokwi part | Real component | Notes |
|---|---|---|
| 3× DS18B20 | 3× DS18B20 (waterproof) + 4.7 kΩ | same OneWire bus — T1 top-centre, T2 bottom-centre, T3 mid wall (detail.md §10) |
| DHT22 | SHT4x / DHT22 in machine tier | ambient reference |
| Green LED | **relay/MOSFET driving the DC compressor** | add flyback diode; compressor on rubber mounts |
| Potentiometer | battery voltage divider (or fuel-gauge IC) | scale to 12 V rail |
| Slide switch | mains/charger presence detect | |
| — | GNSS NEO-M8N / MAX-M10S on **Serial2** (GPIO16/17), PPS to GPIO35 | not in Wokwi; add TinyGPSPlus on real hardware. `GPS_SIM=1` in the sketch fakes a coastal track so the dashboard map demos end to end |
| — | SIM7080 LTE-M on a hardware UART | store-and-forward already in this firmware pattern |
| — | MicroSD / LittleFS log storage | ≥72 h buffer [benchmark] |
| OLED | OLED + web dashboard | the serial CSV becomes the telemetry uplink |

The `control()` function in the sketch is the heart that transfers unchanged: hysteresis + excursion rule + PCM budgeting.

## STEP 9 — Tuning knobs (top of `sketch.ino`)

| Constant | Meaning | Try |
|---|---|---|
| `SIM_SCALE` | time compression | 600 → PCM drains ~10× faster |
| `DEF_ON_T` / `DEF_OFF_T` | hysteresis band defaults | also adjustable live from the dashboard (setpoints) |
| `PCM_FULL_STORY_H` | holdover endurance | set to 12 → see SoC drop faster |
| `BATT_LOW_V / BATT_CRIT_V` | battery alarms | |

## Troubleshooting

| Symptom | Fix |
|---|---|
| OLED blank | check I2C wiring (SDA 21 / SCL 22); address must be 0x3C |
| Temps read −127 (dashboard shows `ERR`) | Wokwi-specific: the probe pin is named **`DQ`** (not `DATA` — a wrong pin name silently drops the wire) and **every DS18B20 needs a unique `deviceID`** — identical IDs collide on the 1-Wire bus. The repo `diagram.json` has both fixed (`DQ` → D4, deviceIDs 111…/222…/333…). Also keep VCC → 3V3, GND → GND and the 4.7 kΩ pull-up (`rpull`) between D4 and 3V3. Startup prints `[DS18B20] N probe(s) found`; after ~5 s of dead bus the box enters `SENSOR-FLT` and recovers by itself once the bus answers |
| DHT reads `nan` for the first seconds | normal — wait one read cycle (~2 s) |
| Compile error on `ledcSetup` | your core is ESP32 v3; the sketch auto-detects, but re-check the `#if` block |
| Libraries not found | install all seven from STEP 3 |
| Serial shows `[MQTT] failed` repeatedly | broker hiccup or blocked network — it retries every 5 s; or set `MQTT_HOST` to `test.mosquitto.org` |
| Dashboard connects but shows nothing | topic prefix in the dashboard must match `TOPIC_BASE` exactly (case-sensitive), and the sim must be running |
| Dashboard won't connect | the broker URL must be a **WebSocket** address (`wss://…`); try `wss://test.mosquitto.org:8081/mqtt` |

### Connection checklist (when Wokwi won't connect)

Work top to bottom — the first item that fails is the cause:

1. **Does the sketch compile?** If the Serial Monitor shows *nothing at all* (not even `[WiFi] connecting...`), the sketch never ran — almost always a missing `PubSubClient` library. Fix: Wokwi Library Manager → add `PubSubClient`, then press play again.
2. **Latest sketch?** MQTT support only exists in the current `sketch.ino`. If yours has no `[WiFi]`/`[MQTT]` lines, paste the latest version.
3. **WiFi block:** expect `[WiFi] connecting to Wokwi-GUEST .... OK` then `[WiFi] IP 10.x.x.x`. On failure it now prints the reason (`no SSID available`, `connect failed`, …) and retries every 10 s automatically. The sketch uses `WiFi.begin("Wokwi-GUEST", "", 6)` — Wokwi-GUEST is fixed to channel 6.
4. **MQTT block:** expect `[MQTT] connecting to broker.hivemq.com:1883 ... connected`. On failure it prints the broker's reason (`connection timeout`, `broker unavailable`, …). Persistent failures → set `MQTT_HOST` to `test.mosquitto.org`.
5. **Topic match:** the dashboard topic prefix must equal `TOPIC_BASE` in the sketch, character for character (case-sensitive).
6. **Dashboard transport:** from GitHub Pages (HTTPS) the broker URL **must** be `wss://broker.hivemq.com:8884/mqtt`. Plain `ws://` is blocked by browsers on HTTPS pages.
7. **Hosting the dashboard yourself?** Upload `liquid-glass.js` next to `dashboard.html` — the page loads it from the same folder. Without it the page still works, just without the glass effect.

## STEP 10 — MQTT & the web dashboard (remote monitor + control)

The sketch connects to a public MQTT broker through Wokwi's virtual WiFi
(`Wokwi-GUEST` = real internet inside the simulator), and `dashboard.html`
talks to the same broker from your browser over WebSocket.

**Topics** (prefix = `TOPIC_BASE` in the sketch, default `coldchain/box001`):

| Topic | Direction | Content |
|---|---|---|
| `<base>/state` | box → dashboard | telemetry JSON every 2 s (temps, state, battery, PCM, excursion, hash, GPS fix) |
| `<base>/event` | box → dashboard | every custody/alarm event + its plain-English note |
| `<base>/settings` | box → dashboard (retained) | compressor mode + setpoints |
| `<base>/status` | box → dashboard (retained) | `online` / `offline` (last will & testament) |
| `<base>/cmd` | dashboard → box | JSON commands |

**Run it:**

1. Start the Wokwi simulation (STEP 4). In the serial log wait for `[MQTT] connected` and the `MQTT LINK UP` event.
2. Double-click `wokwi/dashboard.html` — it opens in your browser, no server needed.
3. Make sure the **topic prefix** matches `TOPIC_BASE`, press **Connect**.
4. Cards fill with live values within ~2 s: chart, state badge, PCM reserve, battery, event log, hash chain, and a location map fed by the GNSS fix (a looping Semarang → Kudus road track while `GPS_SIM=1`; each delivery restarts at the port).

**Remote controls on the dashboard:**

- **Compressor mode** — AUTO / FORCE ON / FORCE OFF. Safety limits still apply: the box refuses force-on without mains power or with a critical battery.
- **Setpoints** — change the hysteresis thresholds live (2.0–7.5 °C, ON must sit above OFF; the box validates and logs rejections as `REMOTE CMD REJECTED`).
- **Mute buzzer** — acknowledged-and-logged; the alarm state itself is unchanged.
- **Custody handover** — logs the event and prints the receipt hash.
- **Refresh now** — asks the box to publish telemetry immediately.
- **Simulator inputs** — five sliders (T1, T2, T3, ambient, battery) plus **Mains** / **Lid** toggle buttons. Every change pins the input over MQTT, same as the scenario buttons; S1 · Normal clears every pin.

**Scenario runner (S1–S8) — drive the demos from the browser:**

The Controls card has a 2×4 grid of scenario buttons. Each sends a `{"cmd":"sim",…}`
command that *pins* a simulated input inside the box — the Wokwi sliders/switches stay
untouched and a pinned value survives until you press S1 (or send `reset`). Every pin is
logged in the tamper-evident chain as `SCENARIO: SIM PINNED` / `SCENARIO: S1 NORMAL`, so
even the demo controls are auditable. Requires the matching firmware — older sketches
answer `REMOTE CMD UNKNOWN`.

| Button | Sends | Expected result |
|---|---|---|
| S1 · Normal | `{"cmd":"sim","reset":1}` | all pins cleared, inputs read the Wokwi parts again, box settles to OK |
| S2 · Hysteresis | `{"cmd":"sim","t1":7.8,"t2":5.8}` — each press steps 7.8 → 5.8 → 4.2 °C | 1st: green LED ON (T1 > ON 7) · 2nd: still ON inside the band (hysteresis lag) · 3rd: OFF below 5. (9 °C was retired — it tripped the 8 °C excursion rule) |
| S3 · Power loss | `{"cmd":"sim","mains":0}` | `PASSIVE-HOLD`, compressor LED off, PCM % drains, HOLD counts down |
| S4 · Door open | `{"cmd":"sim","door":1}` (toggles) | `EVENT: DOOR OPEN`, PCM drain ×1.3 while open; click again to close |
| S5 · Excursion | `{"cmd":"sim","amb":42,"t1":10,"t2":8.5}` | red LED + beeping, EXC climbs to 15 story-min → `QUARANTINE` latch |

The box also protects itself against a dead sensor bus: ~5 s of both probes reading −127
raises `SENSOR-FLT` (red LED + beeper, compressor parked, PCM holds passively) and it
clears by itself once the bus answers. A dashboard pin counts as a live sensor.
| S6 · Battery low | `{"cmd":"sim","batt":10.8}` (cycles 10.8 → 10.3 → 12.4 V) | `BATT-LOW`, compressor shed below 10.5 V, then recovery |
| S7 · Handover | `{"cmd":"handover"}` | `CUSTODY HANDOVER` record + receipt hash |
| S8 · Tamper test | — (flashes the Hash chain card) | edit an old `REC` line in the serial log → every later fingerprint breaks |

Every command is recorded in the tamper-evident chain (`REMOTE CMD: …` records) — remote control is fully auditable, which is exactly the role-based-access argument the essay makes.

**Security (read this):** `broker.hivemq.com` is a public broker — anyone who knows your topic prefix can watch the telemetry or send commands. Change `TOPIC_BASE` to something unique (e.g. `coldchain/yourname-4271`) on **both** the sketch and the dashboard. A real deployment would use an authenticated broker over TLS with per-device keys and role-based access. Set `MQTT_ENABLED` to `0` to run the simulation fully offline.
