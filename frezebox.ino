/* ============================================================
 *  SMART COLD CHAIN BOX — WOKWI SIMULATION (ESP32)
 * ============================================================
 *  Simulates the electronics & software logic of the 30 L cold
 *  chain transport box (see detail.md):
 *
 *   - 2x DS18B20 payload probes on one OneWire bus
 *     (probe 1 = top, probe 2 = bottom; real build has 3)
 *   - DHT22 = ambient air (set its slider to 38-42 C)
 *   - Compressor hysteresis control: ON above 7 C, OFF below 5 C
 *   - Passive holdover: PCM state-of-charge budget + holdover
 *     countdown (full PCM = 24 story-hours at dT = 35 K)
 *   - Excursion rule: >8 C for a cumulative >15 story-minutes
 *     => QUARANTINE latch (brief benchmark)
 *   - Door-open events (HOLD the DOOR button = lid open)
 *   - Battery simulated by the potentiometer (9.0-12.8 V)
 *   - Tamper-evident telemetry: every record prints as one
 *     clearly labeled "REC #" line, and each record shows which
 *     previous record it is chained to (FNV-1a 32-bit demo of
 *     the real Ed25519 signed hash-chain)
 *   - Custody handover events with a shareable receipt hash
 *
 *  LOG FORMAT (human-readable on purpose):
 *    REC #13 | t=00:14 | T1=10.2C T2=4.8C | AMB=42.0C | STATE=DOOR-OPEN
 *            | BAT=12.4V | PCM=98% HOLD=21.0h | EVENT: DOOR OPEN
 *      hash chain: B1C2D3E4  <- links to record #12 (A1B2C3D4)
 *      >> Lid opened - cold air escapes, PCM drain +30% while open.
 *    The hash covers the "REC #" line only; "hash chain" and ">>"
 *    lines are annotations. Events also print a plain-English note.
 *
 *  TIME COMPRESSION: 1 real second = 1 story minute (SIM_SCALE)
 *  -> 15-minute excursion demo = 15 real seconds
 *  -> 24-hour holdover demo    = 24 real minutes
 *
 *  Teaching simulation only — NOT validated firmware.
 * ============================================================ */

#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <DHT.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---------------- Pin map (matches diagram.json) ----------------
#define PIN_ONEWIRE    4    // DS18B20 bus + 4.7k pullup to 3V3
#define PIN_DHT        5    // DHT22 data pin (ambient)
#define PIN_BUZZER    25
#define PIN_LED_COMP  26    // green  = compressor running
#define PIN_LED_ALM   27    // red    = excursion / quarantine
#define PIN_LED_BATT  14    // yellow = battery low
#define BTN_DOOR      32    // HOLD = lid open
#define BTN_MUTE      33    // press = acknowledge alarm
#define BTN_HANDOVER  13    // press = log custody handover
#define PIN_MAINS     23    // slide switch: 3V3 side = power OK, GND side = power lost
#define PIN_BATT_ADC  34    // potentiometer wiper (input-only, ADC1)

// ---------------- Simulation knobs ----------------
const float SIM_SCALE           = 60.0;  // story-seconds per real second
const float COMP_ON_T           = 7.0;   // compressor ON above this (C)
const float COMP_OFF_T          = 5.0;   // compressor OFF below this (C)
const float TEMP_LIMIT          = 8.0;   // upper spec limit [benchmark]
const float EXCURSION_LIMIT_MIN = 15.0;  // max allowed excursion [benchmark]
const float BATT_LOW_V          = 11.0;
const float BATT_CRIT_V         = 10.5;  // compressor shed below this
const float PCM_FULL_STORY_H    = 24.0;  // full PCM endurance at dT=35K [benchmark]
const float PCM_RECHARGE_H      = 8.0;   // re-freeze time (story hours)
const unsigned long LOG_PERIOD_MS  = 2000;
const unsigned long OLED_PERIOD_MS = 400;
const unsigned long DHT_PERIOD_MS  = 2100;

// ---------------- Hardware objects ----------------
OneWire oneWire(PIN_ONEWIRE);
DallasTemperature probes(&oneWire);
DHT dht(PIN_DHT, DHT22);
Adafruit_SSD1306 oled(128, 64, &Wire, -1);

// ---------------- State ----------------
uint8_t  probeCount = 0;
float    t1 = 4.5, t2 = 4.8, tAmb = 40.0, hum = 70.0;
float    battV = 12.4;
bool     mainsOK = true, doorOpen = false, compOn = false;
bool     muted = false, quarantined = false, excActive = false;
float    pcmSoc = 100.0;        // % of cold energy left in the PCM
float    excursionMin = 0.0;    // cumulative story-minutes above 8 C
float    storySec = 0.0;        // story clock (compressed time)
float    holdoverH = 24.0;      // estimated holdover remaining
uint32_t seq = 0;               // log record number
uint32_t prevHash = 2166136261u;// FNV-1a offset basis = chain seed (811C9DC5)
char     lastEvent[28] = "DISPATCH";

struct Btn { uint8_t pin; bool stable = HIGH; bool lastRaw = HIGH; unsigned long tChange = 0; };
Btn btnMute, btnHand;

bool prevDoor = false;
bool prevBattLow = false;
unsigned long lastLoopMs = 0, lastOledMs = 0, lastLogMs = 0;

// ---------------- Utility ----------------
uint32_t fnv1a(const char *s, uint32_t h) {
  while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
  return h;
}

// Story clock as "hh:mm" (compressed shipment time)
void storyClock(char *buf, size_t n) {
  long mins = (long)(storySec / 60.0f);
  snprintf(buf, n, "%02ld:%02ld", mins / 60, mins % 60);
}

void buzzerInit() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  ledcAttach(PIN_BUZZER, 2400, 8);
#else
  ledcSetup(0, 2400, 8);
  ledcAttachPin(PIN_BUZZER, 0);
#endif
}

void buzzerTone(uint32_t freq) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  ledcWriteTone(PIN_BUZZER, freq);
#else
  ledcWriteTone(0, freq);
#endif
}

// Control uses the warmest probe (worst case), alarm uses it too
float tMax() { return fmaxf(t1, t2); }

const char* stateName() {
  if (quarantined)         return "QUARANTINE";
  if (tMax() > TEMP_LIMIT) return "EXCURSION";
  if (!mainsOK)            return "PASSIVE-HOLD";
  if (battV < BATT_LOW_V)  return "BATT-LOW";
  if (doorOpen)            return "DOOR-OPEN";
  return "OK";
}

// ---------------- Tamper-evident log ----------------
// Every record is ONE clearly labeled line starting with "REC #".
// Right below it, the "hash chain" line shows:
//   - the fingerprint (hash) of THIS record
//   - which previous record it links to (with that record's hash)
// The fingerprint = FNV-1a( record line text + previous fingerprint ).
// Because each fingerprint includes the previous one, editing ANY old
// record breaks its fingerprint AND the fingerprint of every record
// after it. (The real device signs the chain with Ed25519 keys.)
// Records with an "EVENT:" field also print a ">>" note that explains
// the event in plain English — the note itself is NOT part of the hash.
void logRecord(const char *eventLabel, const char *humanNote) {
  seq++;
  char tbuf[8];
  storyClock(tbuf, sizeof(tbuf));

  char line[220];
  if (eventLabel != nullptr) {
    snprintf(line, sizeof(line),
             "REC #%lu | t=%s | T1=%.1fC T2=%.1fC | AMB=%.1fC | STATE=%s | BAT=%.1fV | PCM=%.0f%% HOLD=%.1fh | EVENT: %s",
             (unsigned long)seq, tbuf, t1, t2, tAmb, stateName(), battV, pcmSoc, holdoverH, eventLabel);
  } else {
    snprintf(line, sizeof(line),
             "REC #%lu | t=%s | T1=%.1fC T2=%.1fC | AMB=%.1fC | STATE=%s | BAT=%.1fV | PCM=%.0f%% HOLD=%.1fh",
             (unsigned long)seq, tbuf, t1, t2, tAmb, stateName(), battV, pcmSoc, holdoverH);
  }

  uint32_t newHash = fnv1a(line, prevHash);

  if (eventLabel != nullptr) Serial.println("------------------------------------------------------------");
  Serial.println(line);
  if (seq == 1) {
    Serial.printf("  hash chain: %08X  <- first record, seeded from constant 811C9DC5\n", newHash);
  } else {
    Serial.printf("  hash chain: %08X  <- links to record #%lu (%08X)\n",
                  newHash, (unsigned long)(seq - 1), prevHash);
  }
  if (humanNote != nullptr) Serial.printf("  >> %s\n", humanNote);

  prevHash = newHash;
}

void logEvent(const char *name, const char *humanNote) {
  strncpy(lastEvent, name, sizeof(lastEvent) - 1);
  lastEvent[sizeof(lastEvent) - 1] = '\0';
  logRecord(name, humanNote);
}

void custodyHandover() {
  logEvent("CUSTODY HANDOVER",
           "Custody transferred. The receiver recomputes the whole chain and compares it with the receipt hash below.");
  Serial.printf("  RECEIPT: chain head %08X after record #%lu - share this hash with the receiver.\n",
                prevHash, (unsigned long)seq);
}

// ---------------- Control logic ----------------
void control(float dtStorySec) {
  float tmax = tMax();

  // --- compressor hysteresis; requires mains power + healthy battery ---
  bool battOK = battV >= BATT_CRIT_V;
  if (!mainsOK || !battOK)    compOn = false;
  else if (tmax >= COMP_ON_T) compOn = true;
  else if (tmax <= COMP_OFF_T) compOn = false;
  digitalWrite(PIN_LED_COMP, compOn ? HIGH : LOW);
  digitalWrite(PIN_LED_BATT, (battV < BATT_LOW_V) ? HIGH : LOW);

  // --- PCM state of charge ---
  // Heat load is proportional to dT; full PCM lasts 24 story-hours at dT=35K.
  // An open lid adds ~30% load. Compressor re-freezes PCM when payload <= 5.5 C.
  float dT = tAmb - tmax;
  if (dT < 0) dT = 0;
  float drainPerH = (100.0f / PCM_FULL_STORY_H) * (dT / 35.0f);
  if (doorOpen) drainPerH *= 1.3f;
  float rechPerH = (compOn && tmax <= 5.5f) ? (100.0f / PCM_RECHARGE_H) : 0.0f;
  pcmSoc += (rechPerH - drainPerH) * (dtStorySec / 3600.0f);
  if (pcmSoc > 100) pcmSoc = 100;
  if (pcmSoc < 0)   pcmSoc = 0;
  holdoverH = (drainPerH > 0.05f) ? (pcmSoc / drainPerH) : 99.0f;

  // --- excursion rule: cumulative >15 story-min above 8 C => QUARANTINE ---
  if (tmax > TEMP_LIMIT) {
    excursionMin += dtStorySec / 60.0f;
    if (!excActive) {
      excActive = true;
      logEvent("EXCURSION START",
               "Payload is above the 8.0C limit. The excursion timer is running; at 15 story-minutes TOTAL the batch is quarantined.");
    }
  } else {
    excActive = false;
  }
  if (!quarantined && excursionMin >= EXCURSION_LIMIT_MIN) {
    quarantined = true;
    logEvent("QUARANTINE FLAGGED",
             "Payload spent more than 15 story-minutes above 8C. The batch needs a product-specific QA disposition before it may be used.");
  }
  digitalWrite(PIN_LED_ALM, (tmax > TEMP_LIMIT || quarantined) ? HIGH : LOW);
}

// ---------------- Local alarm (buzzer pattern, non-blocking) ----------------
void beepTask() {
  bool alarm = (tMax() > TEMP_LIMIT) || quarantined ||
               (battV < BATT_CRIT_V) || (!mainsOK && pcmSoc < 15.0f);
  if (!alarm) { muted = false; buzzerTone(0); return; }
  if (muted)  { buzzerTone(0); return; }
  buzzerTone((millis() % 900) < 120 ? 2400 : 0);
}

// ---------------- Display ----------------
void drawOled() {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  oled.printf("COLD-CHAIN %-11s", stateName());
  oled.setCursor(0, 12);
  oled.printf("T1 %5.1f  T2 %5.1f", t1, t2);
  oled.setCursor(0, 22);
  oled.printf("AMB %.0fC  HUM %.0f%%", tAmb, hum);
  oled.setCursor(0, 32);
  oled.printf("CMP %s   BAT %4.1fV", compOn ? "ON " : "OFF", battV);
  oled.setCursor(0, 42);
  oled.printf("PCM %3.0f%% HOLD %4.1fh", pcmSoc, holdoverH);
  oled.setCursor(0, 52);
  oled.printf("EXC %4.1f/%2.0fm #%lu", excursionMin, EXCURSION_LIMIT_MIN, (unsigned long)seq);
  oled.display();
}

// ---------------- Buttons ----------------
bool pressedEdge(Btn &b) {
  bool raw = (digitalRead(b.pin) == LOW);
  unsigned long now = millis();
  if (raw != b.lastRaw) { b.lastRaw = raw; b.tChange = now; }
  else if ((now - b.tChange) > 40 && raw != b.stable) {
    b.stable = raw;
    if (raw) return true;
  }
  return false;
}

// ---------------- Setup / Loop ----------------
void setup() {
  Serial.begin(115200);

  pinMode(PIN_LED_COMP, OUTPUT);
  pinMode(PIN_LED_ALM,  OUTPUT);
  pinMode(PIN_LED_BATT, OUTPUT);
  pinMode(BTN_DOOR,     INPUT_PULLUP);
  pinMode(BTN_MUTE,     INPUT_PULLUP);
  pinMode(BTN_HANDOVER, INPUT_PULLUP);
  pinMode(PIN_MAINS,    INPUT);   // driven by slide switch to 3V3 or GND
  btnMute.pin = BTN_MUTE;
  btnHand.pin = BTN_HANDOVER;
  buzzerInit();

  Wire.begin(21, 22);
  oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  dht.begin();

  probes.begin();
  probeCount = probes.getDeviceCount();
  probes.setResolution(12);
  probes.setWaitForConversion(false);

  Serial.println();
  Serial.println("==========================================================");
  Serial.println("  SMART COLD CHAIN BOX - WOKWI SIMULATION (ESP32)");
  Serial.println("==========================================================");
  Serial.println("Time compression: 1 real second = 1 story-minute");
  Serial.println("Telemetry: 1 record every 2 real seconds (= 2 story-min)");
  Serial.println();
  Serial.println("HOW TO READ THE LOG");
  Serial.println("-------------------");
  Serial.println("Every record is ONE line starting with 'REC #':");
  Serial.println("  REC #13 | t=00:14 | T1=10.2C T2=4.8C | AMB=42.0C | STATE=DOOR-OPEN | BAT=12.4V | PCM=98% HOLD=21.0h | EVENT: DOOR OPEN");
  Serial.println("  Field meaning:");
  Serial.println("    REC #  : record number - never repeats, never reorders");
  Serial.println("    t      : story clock hh:mm (compressed shipment time)");
  Serial.println("    T1/T2  : payload probe temperatures (T1 top, T2 bottom)");
  Serial.println("    AMB    : ambient (outside) air temperature");
  Serial.println("    STATE  : OK | DOOR-OPEN | BATT-LOW | PASSIVE-HOLD | EXCURSION | QUARANTINE");
  Serial.println("    BAT    : battery voltage (LOW < 11.0V, compressor shed < 10.5V)");
  Serial.println("    PCM    : cold energy left in the phase-change material (%)");
  Serial.println("    HOLD   : estimated passive holdover remaining (hours)");
  Serial.println();
  Serial.println("TAMPER-EVIDENT CHAIN");
  Serial.println("--------------------");
  Serial.println("  hash chain: <THIS record's fingerprint> <- links to record #<previous> (its fingerprint)");
  Serial.println("Each fingerprint = FNV-1a of the REC line text + the PREVIOUS fingerprint.");
  Serial.println("Edit any old record and every later fingerprint stops matching -");
  Serial.println("the same property the real device gets from a signed hash chain.");
  Serial.println("Lines starting with '>>' are plain-English explanations (not hashed).");
  Serial.println();
  Serial.println("Play the physics: drag the DS18B20 / DHT22 temperature sliders,");
  Serial.println("flip the slide switch (power), turn the pot (battery), use buttons.");
  Serial.println("==========================================================");
  logEvent("DISPATCH",
           "Shipment handed to the courier. Monitoring and the custody chain start here.");
  drawOled();
}

void loop() {
  unsigned long now = millis();
  float dtReal = (now - lastLoopMs) / 1000.0f;
  if (dtReal < 0)    dtReal = 0;
  if (dtReal > 5.0f) dtReal = 5.0f;
  lastLoopMs = now;
  float dtStorySec = dtReal * SIM_SCALE;
  storySec += dtStorySec;

  // ---- digital inputs ----
  mainsOK  = (digitalRead(PIN_MAINS) == HIGH);
  doorOpen = (digitalRead(BTN_DOOR) == LOW);
  if (doorOpen != prevDoor) {
    if (doorOpen) logEvent("DOOR OPEN",
                           "Lid opened - cold air escapes, PCM drain rate is +30% while it stays open.");
    else          logEvent("DOOR CLOSED",
                           "Lid closed again - PCM drain rate back to normal.");
    prevDoor = doorOpen;
  }
  if (pressedEdge(btnMute)) {
    muted = true;
    logEvent("ALARM MUTE",
             "Operator acknowledged the buzzer. The alarm state itself is unchanged - this entry exists for audit.");
  }
  if (pressedEdge(btnHand)) custodyHandover();

  // ---- battery voltage sim (potentiometer) ----
  int raw = analogRead(PIN_BATT_ADC);              // 0..4095
  battV = 9.0f + (raw / 4095.0f) * 3.8f;           // -> 9.0..12.8 V
  bool battLow = (battV < BATT_LOW_V);
  if (battLow != prevBattLow) {
    if (battLow) logEvent("BATTERY LOW",
                          "Battery below 11.0V - yellow LED on. Compressor still allowed until 10.5V.");
    else         logEvent("BATTERY RECOVERED",
                          "Battery back above 11.0V - low-battery warning cleared.");
    prevBattLow = battLow;
  }

  // ---- DS18B20 conversion state machine (non-blocking) ----
  static bool convPending = false;
  static unsigned long convStart = 0, lastProbeReq = 0;
  if (!convPending && (now - lastProbeReq) >= 1000) {
    probes.requestTemperatures();
    convPending = true;
    convStart = now;
  }
  if (convPending && (now - convStart) >= 750) {
    convPending = false;
    lastProbeReq = now;
    t1 = probes.getTempCByIndex(0);
    t2 = (probeCount > 1) ? probes.getTempCByIndex(1) : t1;
  }

  // ---- DHT22 ambient every ~2 s ----
  static unsigned long lastDht = 0;
  if ((now - lastDht) >= DHT_PERIOD_MS) {
    lastDht = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) tAmb = t;
    if (!isnan(h)) hum = h;
  }

  control(dtStorySec);
  beepTask();

  if (now - lastOledMs >= OLED_PERIOD_MS) { lastOledMs = now; drawOled(); }
  if (now - lastLogMs  >= LOG_PERIOD_MS)  { lastLogMs  = now; logRecord(nullptr, nullptr); }
}
