/* ============================================================
 *  SMART COLD CHAIN BOX — WOKWI SIMULATION (ESP32)
 *  with MQTT telemetry + remote control
 * ============================================================
 *  Simulates the electronics & software logic of the 30 L cold
 *  chain transport box (see detail.md):
 *
 *   - 3x DS18B20 payload probes on one OneWire bus
 *     (T1 top-centre, T2 bottom-centre, T3 mid wall - detail.md §10)
 *   - DHT22 = ambient air (set its slider to 38-42 C)
 *   - Compressor hysteresis control (defaults: ON > 7 C, OFF < 5 C,
 *     adjustable live from the web dashboard)
 *   - Passive holdover: PCM state-of-charge budget + holdover
 *     countdown (full PCM = 24 story-hours at dT = 35 K)
 *   - Excursion rule: >8 C for a cumulative >15 story-minutes
 *     => QUARANTINE latch (brief benchmark)
 *   - Door-open events (HOLD the DOOR button = lid open)
 *   - Battery simulated by the potentiometer (9.0-12.8 V)
 *   - Tamper-evident telemetry: every record prints as one
 *     clearly labeled "REC #" line chained by visible hashes
 *   - MQTT: publishes telemetry/events, accepts remote commands
 *     (compressor mode, setpoints, mute, custody handover)
 *
 *  NETWORKING (Wokwi): the virtual WiFi "Wokwi-GUEST" gives the
 *  simulated ESP32 real internet access, so it talks to a public
 *  MQTT broker. The web dashboard (dashboard.html) connects to
 *  the SAME broker over WebSocket from your browser.
 *
 *    <base>/state     box -> dashboard   telemetry JSON every 2 s
 *    <base>/event     box -> dashboard   events + plain-English note
 *    <base>/settings  box -> dashboard   mode + setpoints (retained)
 *    <base>/status    box -> dashboard   online/offline (retained LWT)
 *    <base>/cmd       dashboard -> box   JSON commands
 *
 *  !! PUBLIC BROKER: change TOPIC_BASE to something unique.
 *  !! Set MQTT_ENABLED to 0 to run the sim fully offline.
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
#include <WiFi.h>
#include <PubSubClient.h>

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
const float DEF_ON_T            = 7.0;   // default compressor ON threshold (C)
const float DEF_OFF_T           = 5.0;   // default compressor OFF threshold (C)
const float TEMP_LIMIT          = 8.0;   // upper spec limit [benchmark]
const float EXCURSION_LIMIT_MIN = 15.0;  // max allowed excursion [benchmark]
const float BATT_LOW_V          = 11.0;
const float BATT_CRIT_V         = 10.5;  // compressor shed below this
const float PCM_FULL_STORY_H    = 24.0;  // full PCM endurance at dT=35K [benchmark]
const float PCM_RECHARGE_H      = 8.0;   // re-freeze time (story hours)
const unsigned long LOG_PERIOD_MS  = 2000;
const unsigned long OLED_PERIOD_MS = 400;
const unsigned long DHT_PERIOD_MS  = 2100;

// ---------------- WiFi & MQTT ----------------
#define MQTT_ENABLED 1                  // 0 = run fully offline
const char* WIFI_SSID  = "Wokwi-GUEST"; // Wokwi's virtual internet WiFi
const char* WIFI_PASS  = "";
const char* MQTT_HOST  = "broker.hivemq.com";   // alternative: test.mosquitto.org
const uint16_t MQTT_PORT = 1883;
// !! PUBLIC broker — make this unique, e.g. "coldchain/budi-7381" !!
const char* TOPIC_BASE = "coldchain/box001";

char topicState[80], topicEvent[80], topicCmd[80], topicStatus[80], topicSettings[80];
char clientId[24];

// ---------------- GNSS ----------------
// Wokwi has no GPS part: GPS_SIM=1 walks a pre-defined waypoint route so the
// dashboard map shows a realistic road track. Real hardware: NEO-M8N / MAX-M10S
// on Serial2 (GPIO16 RX / GPIO17 TX) with TinyGPSPlus, PPS on GPIO35.
//
// Route: Simongan Port Semarang → Puskesmas Kaliwungu → Puskesmas Weleri → Kudus
// All points are on real roads (Pantura / Jl. Raya Semarang-Demak-Kudus corridor).
// Each step ≈ 200–400 m apart at ~30 km/h simulated.
#define GPS_SIM 1

struct WP { float lat; float lon; };
static const WP GPS_ROUTE[] = {
  // Pelabuhan Simongan / DISPATCH point
  { -6.9550f, 110.4240f },
  { -6.9521f, 110.4268f },
  { -6.9493f, 110.4312f },
  { -6.9470f, 110.4375f },
  // Jl. Yos Sudarso → menuju Pantura timur
  { -6.9452f, 110.4445f },
  { -6.9438f, 110.4530f },
  { -6.9432f, 110.4620f },
  // Jl. Raya Demak
  { -6.9429f, 110.4740f },
  { -6.9424f, 110.4880f },
  { -6.9418f, 110.5010f },
  { -6.9408f, 110.5180f },
  // Kota Demak
  { -6.9392f, 110.5370f },
  { -6.9380f, 110.5530f },
  { -6.9368f, 110.5710f },
  // Pantura Demak → Kudus
  { -6.9352f, 110.5940f },
  { -6.9330f, 110.6190f },
  { -6.9305f, 110.6450f },
  { -6.9278f, 110.6720f },
  { -6.9248f, 110.7000f },
  { -6.9215f, 110.7280f },
  { -6.9180f, 110.7540f },
  // Masuk Kudus kota
  { -6.9150f, 110.7750f },
  { -6.9125f, 110.7930f },
  // Puskesmas Kudus / DELIVERY point (tujuan akhir)
  { -6.9105f, 110.8090f },
};
static const int GPS_ROUTE_LEN = (int)(sizeof(GPS_ROUTE) / sizeof(GPS_ROUTE[0]));
static int  gWpIdx   = 0;     // current waypoint index (loops: port -> Kudus -> port)
float   gLat  = GPS_ROUTE[0].lat;
float   gLon  = GPS_ROUTE[0].lon;
float   gKph  = 30.0f;
uint8_t gSats = 9;
WiFiClient   net;
PubSubClient mqtt(net);
unsigned long lastMqttAttempt = 0;

const uint8_t MODE_AUTO = 0, MODE_ON = 1, MODE_OFF = 2;
uint8_t compMode = MODE_AUTO;
float   setOnT  = DEF_ON_T;    // live-adjustable from the dashboard
float   setOffT = DEF_OFF_T;

// ---------------- Hardware objects ----------------
OneWire oneWire(PIN_ONEWIRE);
DallasTemperature probes(&oneWire);
DHT dht(PIN_DHT, DHT22);
Adafruit_SSD1306 oled(128, 64, &Wire, -1);

// ---------------- State ----------------
uint8_t  probeCount = 0;
float    t1 = 4.5, t2 = 4.8, t3 = 4.9, tAmb = 40.0, hum = 70.0;
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
bool    sensorFault = false;    // both payload probes silent (-127)
uint8_t faultReads = 0;         // consecutive failed conversions (debounce)
unsigned long lastLoopMs = 0, lastOledMs = 0, lastLogMs = 0;

// ---------------- Dashboard scenario pins (S1-S8) ----------------
// A {"cmd":"sim",...} command from the web dashboard pins one or more
// simulated inputs so the demo scenarios can be driven from the browser.
// NAN / -1 = not pinned: the loop then reads the Wokwi part as usual.
float  simT1 = NAN, simT2 = NAN, simT3 = NAN, simTamb = NAN, simBatt = NAN;
int8_t simMains = -1, simDoor = -1;

void applySimOverrides() {
  if (!isnan(simT1))   t1    = simT1;
  if (!isnan(simT2))   t2    = simT2;
  if (!isnan(simT3))   t3    = simT3;
  if (!isnan(simTamb)) tAmb  = simTamb;
  if (!isnan(simBatt)) battV = simBatt;
  if (simMains >= 0)   mainsOK  = (simMains == 1);
  if (simDoor  >= 0)   doorOpen = (simDoor  == 1);
}

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
float tMax() { return fmaxf(t1, fmaxf(t2, t3)); }

const char* stateName() {
  if (quarantined)         return "QUARANTINE";
  if (sensorFault)         return "SENSOR-FLT";
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
             "REC #%lu | t=%s | T1=%.1fC T2=%.1fC T3=%.1fC | AMB=%.1fC | STATE=%s | BAT=%.1fV | PCM=%.0f%% HOLD=%.1fh | EVENT: %s",
             (unsigned long)seq, tbuf, t1, t2, t3, tAmb, stateName(), battV, pcmSoc, holdoverH, eventLabel);
  } else {
    snprintf(line, sizeof(line),
             "REC #%lu | t=%s | T1=%.1fC T2=%.1fC T3=%.1fC | AMB=%.1fC | STATE=%s | BAT=%.1fV | PCM=%.0f%% HOLD=%.1fh",
             (unsigned long)seq, tbuf, t1, t2, t3, tAmb, stateName(), battV, pcmSoc, holdoverH);
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

// ---------------- MQTT publish helpers ----------------
void publishTelemetry() {
  if (!mqtt.connected()) return;
  char tbuf[8];
  storyClock(tbuf, sizeof(tbuf));
#if GPS_SIM
  // Advance one waypoint along the road route; at Kudus the shipment is
  // complete and the next one starts at the port (loop, no ping-pong).
  gWpIdx++;
  if (gWpIdx >= GPS_ROUTE_LEN) {
    gWpIdx = 0;
    logEvent("DELIVERY COMPLETE",
             "Box reached Puskesmas Kudus. The custody chain keeps the full history; a fresh shipment starts at Simongan port.");
  }
  gLat = GPS_ROUTE[gWpIdx].lat;
  gLon = GPS_ROUTE[gWpIdx].lon;
  // vary speed slightly (25–35 km/h) to look realistic
  gKph = 25.0f + (float)(seq % 5) * 2.0f;
#endif
  char buf[380];
  snprintf(buf, sizeof(buf),
           "{\"seq\":%lu,\"t\":\"%s\",\"t1\":%.2f,\"t2\":%.2f,\"t3\":%.2f,\"amb\":%.1f,"
           "\"state\":\"%s\",\"batt\":%.2f,\"pcm\":%.1f,\"hold\":%.1f,"
           "\"exc\":%.1f,\"comp\":%s,\"hash\":\"%08X\","
           "\"lat\":%.4f,\"lon\":%.4f,\"kph\":%.0f,\"sat\":%u}",
           (unsigned long)seq, tbuf, t1, t2, t3, tAmb, stateName(), battV,
           pcmSoc, holdoverH, excursionMin, compOn ? "true" : "false", prevHash,
           gLat, gLon, gKph, (unsigned)gSats);
  if (!mqtt.publish(topicState, buf))
    Serial.println("[MQTT] telemetry publish FAILED (link dropped or buffer too small)");
}

void publishEvent(const char *name, const char *note) {
  if (!mqtt.connected()) return;
  char tbuf[8];
  storyClock(tbuf, sizeof(tbuf));
  char buf[240];
  snprintf(buf, sizeof(buf), "{\"seq\":%lu,\"t\":\"%s\",\"event\":\"%s\",\"note\":\"%s\"}",
           (unsigned long)seq, tbuf, name, note);
  if (!mqtt.publish(topicEvent, buf))
    Serial.println("[MQTT] event publish FAILED (link dropped or buffer too small)");
}

void publishStatus(const char *s) {
  mqtt.publish(topicStatus, s, true);
}

void publishSettings() {
  if (!mqtt.connected()) return;
  char buf[96];
  const char* m = (compMode == MODE_AUTO) ? "auto" : (compMode == MODE_ON) ? "on" : "off";
  snprintf(buf, sizeof(buf), "{\"mode\":\"%s\",\"onT\":%.1f,\"offT\":%.1f}", m, setOnT, setOffT);
  mqtt.publish(topicSettings, buf, true);
}

void logEvent(const char *name, const char *humanNote) {
  strncpy(lastEvent, name, sizeof(lastEvent) - 1);
  lastEvent[sizeof(lastEvent) - 1] = '\0';
  logRecord(name, humanNote);
  publishEvent(name, humanNote);
}

void custodyHandover() {
  logEvent("CUSTODY HANDOVER",
           "Custody transferred. The receiver recomputes the whole chain and compares it with the receipt hash below.");
  Serial.printf("  RECEIPT: chain head %08X after record #%lu - share this hash with the receiver.\n",
                prevHash, (unsigned long)seq);
}

// ---------------- MQTT commands ----------------
// Minimal JSON field readers for our tiny command schema (teaching code):
//   {"cmd":"mode","value":"auto"}   {"cmd":"mode","value":"on"}   {"cmd":"mode","value":"off"}
//   {"cmd":"setpoints","on":7.0,"off":5.0}
//   {"cmd":"mute"}   {"cmd":"handover"}   {"cmd":"ping"}
//   {"cmd":"sim","reset":1}                  <- clear every scenario pin (S1)
//   {"cmd":"sim","t1":9,"t2":6,"t3":5}        <- pin payload probes (S2)
//   {"cmd":"sim","amb":42,"t1":10,"t2":8.5}  <- excursion setup (S5)
//   {"cmd":"sim","batt":10.8}                <- pin battery voltage (S6)
//   {"cmd":"sim","mains":0|1}  {"cmd":"sim","door":0|1}   <- S3 / S4
float jsonNum(const char *j, const char *key, float def) {
  char pat[24];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(j, pat);
  if (!p) return def;
  p += strlen(pat);
  while (*p == ' ' || *p == ':') p++;
  return strtof(p, nullptr);
}

void jsonStr(const char *j, const char *key, char *out, size_t n) {
  char pat[24];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  out[0] = '\0';
  const char *p = strstr(j, pat);
  if (!p) return;
  p += strlen(pat);
  while (*p == ' ' || *p == ':') p++;
  if (*p != '"') return;
  p++;
  size_t i = 0;
  while (*p && *p != '"' && i + 1 < n) out[i++] = *p++;
  out[i] = '\0';
}

void applyCommand(const char *msg) {
  char cmdName[16], value[12], note[160];
  jsonStr(msg, "cmd", cmdName, sizeof(cmdName));

  if (strcmp(cmdName, "mode") == 0) {
    jsonStr(msg, "value", value, sizeof(value));
    if (strcmp(value, "auto") == 0) {
      compMode = MODE_AUTO;
      snprintf(note, sizeof(note),
               "Compressor controller returned to AUTO (hysteresis: start above %.1fC, stop below %.1fC).",
               setOnT, setOffT);
      logEvent("REMOTE CMD: MODE=AUTO", note);
    } else if (strcmp(value, "on") == 0) {
      compMode = MODE_ON;
      logEvent("REMOTE CMD: MODE=ON",
               "Compressor forced ON from the dashboard. The box refuses force-on without mains power or with a critical battery.");
    } else if (strcmp(value, "off") == 0) {
      compMode = MODE_OFF;
      logEvent("REMOTE CMD: MODE=OFF",
               "Compressor forced OFF from the dashboard. Passive holdover clock keeps running.");
    } else {
      logEvent("REMOTE CMD REJECTED", "Unknown compressor mode value (use auto/on/off). Settings unchanged.");
      publishSettings();
    }
    publishSettings();

  } else if (strcmp(cmdName, "setpoints") == 0) {
    float on  = jsonNum(msg, "on",  NAN);
    float off = jsonNum(msg, "off", NAN);
    if (isnan(on) || isnan(off)) {
      logEvent("REMOTE CMD REJECTED", "Setpoints command is missing numbers. Settings unchanged.");
      publishSettings();
      return;
    }
    if (on <= off || on < 2.0f || on > 7.5f || off < 2.0f || off > 7.0f) {
      logEvent("REMOTE CMD REJECTED",
               "Invalid setpoints: both must be 2.0-7.5C and the ON threshold must sit above the OFF one.");
      publishSettings();
      return;
    }
    setOnT = on;
    setOffT = off;
    snprintf(note, sizeof(note),
             "Hysteresis thresholds updated: compressor starts above %.1fC and stops below %.1fC.",
             setOnT, setOffT);
    logEvent("REMOTE CMD: SETPOINTS", note);
    publishSettings();

  } else if (strcmp(cmdName, "mute") == 0) {
    muted = true;
    logEvent("REMOTE CMD: MUTE",
             "Alarm acknowledged from the dashboard. The alarm state itself is unchanged - this entry exists for audit.");

  } else if (strcmp(cmdName, "handover") == 0) {
    custodyHandover();

  } else if (strcmp(cmdName, "ping") == 0) {
    publishTelemetry();   // refresh now, no log noise

  } else if (strcmp(cmdName, "sim") == 0) {
    // Scenario runner (S1-S8): pin one or more simulated inputs.
    // Missing fields keep their current value; reset:1 clears every pin.
    float  v;
    char   tmp[20], what[110] = "";
    bool   any = false, reset = false;

    if (jsonNum(msg, "reset", 0) > 0.5f) {
      simT1 = simT2 = simT3 = simTamb = simBatt = NAN;
      simMains = simDoor = -1;
      any = reset = true;
    }
    v = jsonNum(msg, "t1", NAN);
    if (!isnan(v)) { simT1 = v; any = true;
      snprintf(tmp, sizeof(tmp), "T1=%.1fC ", v); strlcat(what, tmp, sizeof(what)); }
    v = jsonNum(msg, "t2", NAN);
    if (!isnan(v)) { simT2 = v; any = true;
      snprintf(tmp, sizeof(tmp), "T2=%.1fC ", v); strlcat(what, tmp, sizeof(what)); }
    v = jsonNum(msg, "t3", NAN);
    if (!isnan(v)) { simT3 = v; any = true;
      snprintf(tmp, sizeof(tmp), "T3=%.1fC ", v); strlcat(what, tmp, sizeof(what)); }
    v = jsonNum(msg, "amb", NAN);
    if (!isnan(v)) { simTamb = v; any = true;
      snprintf(tmp, sizeof(tmp), "AMB=%.1fC ", v); strlcat(what, tmp, sizeof(what)); }
    v = jsonNum(msg, "batt", NAN);
    if (!isnan(v)) { simBatt = v; any = true;
      snprintf(tmp, sizeof(tmp), "BAT=%.1fV ", v); strlcat(what, tmp, sizeof(what)); }
    v = jsonNum(msg, "mains", -1);
    if (v >= 0) { simMains = (v >= 0.5f); any = true;
      strlcat(what, simMains ? "MAINS=OK " : "MAINS=LOST ", sizeof(what)); }
    v = jsonNum(msg, "door", -1);
    if (v >= 0) { simDoor = (v >= 0.5f); any = true;
      strlcat(what, simDoor ? "LID=OPEN " : "LID=CLOSED ", sizeof(what)); }

    if (!any) {
      logEvent("REMOTE CMD REJECTED",
               "Sim command without a recognized field (reset/t1/t2/amb/batt/mains/door). Nothing changed.");
      return;
    }
    applySimOverrides();
    if (reset) {
      logEvent("SCENARIO: S1 NORMAL",
               "All scenario pins cleared - inputs come from the Wokwi parts again. The box should settle back to OK.");
    } else {
      snprintf(note, sizeof(note),
               "Dashboard pinned simulated inputs: %s- logged in this chain like any real input.", what);
      logEvent("SCENARIO: SIM PINNED", note);
    }

  } else {
    logEvent("REMOTE CMD UNKNOWN",
             "Command not recognized - ignored. The dashboard and the box firmware may be out of sync.");
  }
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  char msg[160];
  unsigned int n = (length < sizeof(msg) - 1) ? length : sizeof(msg) - 1;
  memcpy(msg, payload, n);
  msg[n] = '\0';
  Serial.printf("[MQTT] command on %s -> %s\n", topic, msg);
  applyCommand(msg);
}

// Readable status names so the Serial log explains itself
const char* wifiStatusName(int s) {
  switch (s) {
    case WL_CONNECTED:      return "connected";
    case WL_NO_SSID_AVAIL:  return "no SSID available";
    case WL_CONNECT_FAILED: return "connect failed";
    case WL_CONNECTION_LOST:return "connection lost";
    case WL_DISCONNECTED:   return "disconnected";
    case WL_IDLE_STATUS:    return "idle";
    case WL_SCAN_COMPLETED: return "scan completed";
    default:                return "unknown";
  }
}

const char* mqttStateName(int s) {
  switch (s) {
    case MQTT_CONNECTION_TIMEOUT:    return "connection timeout";
    case MQTT_CONNECTION_LOST:       return "connection lost";
    case MQTT_CONNECT_FAILED:        return "TCP connect failed";
    case MQTT_DISCONNECTED:          return "disconnected";
    case MQTT_CONNECTED:             return "connected";
    case MQTT_CONNECT_BAD_PROTOCOL:  return "bad protocol";
    case MQTT_CONNECT_BAD_CLIENT_ID: return "bad client id";
    case MQTT_CONNECT_UNAVAILABLE:   return "broker unavailable";
    case MQTT_CONNECT_BAD_CREDENTIALS: return "bad credentials";
    case MQTT_CONNECT_UNAUTHORIZED:  return "unauthorized";
    default:                         return "unknown";
  }
}

// ---------------- MQTT connection task (non-blocking, self-healing) ----------------
void netTask() {
#if MQTT_ENABLED
  // --- WiFi keep-alive: if the link is down, re-attempt every 10 s ---
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastWifiTry = 0;
    if (millis() - lastWifiTry > 10000) {
      lastWifiTry = millis();
      Serial.printf("[WiFi] not connected (%s) - retrying...\n", wifiStatusName(WiFi.status()));
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS, 6);   // Wokwi-GUEST lives on channel 6
    }
    return;                                   // no WiFi -> no MQTT attempt yet
  }

  // --- MQTT connect / reconnect every 5 s ---
  if (!mqtt.connected()) {
    if (millis() - lastMqttAttempt > 5000) {
      lastMqttAttempt = millis();
      Serial.printf("[MQTT] connecting to %s:%u ... ", MQTT_HOST, MQTT_PORT);
      // last will: retained "offline" so dashboards know when the box drops
      if (mqtt.connect(clientId, topicStatus, 1, true, "offline")) {
        Serial.println("connected");
        mqtt.subscribe(topicCmd);
        publishStatus("online");
        publishSettings();                    // retained, so late joiners sync
        logEvent("MQTT LINK UP",
                 "Telemetry and remote control are now live on the web dashboard.");
      } else {
        Serial.printf("failed (%s) - retrying in 5 s\n", mqttStateName(mqtt.state()));
      }
    }
    return;
  }

  mqtt.loop();
#endif
}

// ---------------- Control logic ----------------
void control(float dtStorySec) {
  float tmax = tMax();

  // --- compressor: dashboard mode may force, safety limits still apply ---
  bool battOK  = battV >= BATT_CRIT_V;
  bool powerOK = mainsOK && battOK;
  if (sensorFault)              compOn = false;        // blind: PCM holds passively, no force-on
  else if (compMode == MODE_OFF) compOn = false;
  else if (compMode == MODE_ON) compOn = powerOK;      // cannot force without power
  else {
    if (!powerOK)               compOn = false;        // AUTO hysteresis
    else if (tmax >= setOnT)    compOn = true;
    else if (tmax <= setOffT)   compOn = false;
  }
  digitalWrite(PIN_LED_COMP, compOn ? HIGH : LOW);
  digitalWrite(PIN_LED_BATT, (battV < BATT_LOW_V) ? HIGH : LOW);

  // --- PCM state of charge ---
  // Heat load is proportional to dT; full PCM lasts 24 story-hours at dT=35K.
  // An open lid adds ~30% load. Compressor re-freezes PCM when payload is cold.
  float dT = tAmb - tmax;
  if (dT < 0) dT = 0;
  float drainPerH = (100.0f / PCM_FULL_STORY_H) * (dT / 35.0f);
  if (doorOpen) drainPerH *= 1.3f;
  float rechPerH = (compOn && tmax <= setOffT + 0.5f) ? (100.0f / PCM_RECHARGE_H) : 0.0f;
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
  digitalWrite(PIN_LED_ALM, (tmax > TEMP_LIMIT || quarantined || sensorFault) ? HIGH : LOW);
}

// ---------------- Local alarm (buzzer pattern, non-blocking) ----------------
void beepTask() {
  bool alarm = (tMax() > TEMP_LIMIT) || quarantined || sensorFault ||
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
  oled.printf("T1%4.1f T2%4.1f T3%4.1f", t1, t2, t3);
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
  Serial.printf("[DS18B20] %d probe(s) found on the OneWire bus (GPIO %d)\n",
                probeCount, PIN_ONEWIRE);
  if (probeCount == 0)
    Serial.println("[DS18B20] !! No probe answered - temps will read -127. Check in Wokwi: "
                   "every probe uses pin DQ (not DATA) -> D4, has a unique deviceID attribute, "
                   "VCC -> 3V3, GND -> GND, plus the 4.7k pull-up (rpull) between D4 and 3V3.");

  // ---- MQTT topics & connection ----
  snprintf(topicState,    sizeof(topicState),    "%s/state",    TOPIC_BASE);
  snprintf(topicEvent,    sizeof(topicEvent),    "%s/event",    TOPIC_BASE);
  snprintf(topicCmd,      sizeof(topicCmd),      "%s/cmd",      TOPIC_BASE);
  snprintf(topicStatus,   sizeof(topicStatus),   "%s/status",   TOPIC_BASE);
  snprintf(topicSettings, sizeof(topicSettings), "%s/settings", TOPIC_BASE);
#if MQTT_ENABLED
  Serial.printf("[WiFi] connecting to %s ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);   // Wokwi-GUEST: fixed channel 6, per Wokwi docs
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(250);
    Serial.print('.');
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" OK");
    Serial.printf("[WiFi] IP %s - internet access via Wokwi gateway\n",
                  WiFi.localIP().toString().c_str());
  } else {
    Serial.printf(" FAILED (%s) - will keep retrying every 10 s\n",
                  wifiStatusName(WiFi.status()));
  }
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512);
  snprintf(clientId, sizeof(clientId), "ccbox-%08X", (uint32_t)esp_random());
#endif

  Serial.println();
  Serial.println("==========================================================");
  Serial.println("  SMART COLD CHAIN BOX - WOKWI SIMULATION (ESP32)");
  Serial.println("==========================================================");
  Serial.println("Time compression: 1 real second = 1 story-minute");
  Serial.println("Telemetry: 1 record every 2 real seconds (= 2 story-min)");
#if MQTT_ENABLED
  Serial.printf("MQTT: %s:%d | topics under \"%s\" (state/event/settings/status/cmd)\n",
                MQTT_HOST, MQTT_PORT, TOPIC_BASE);
  Serial.println("Open wokwi/dashboard.html in your browser, set the same topic prefix, Connect.");
#endif
  Serial.println();
  Serial.println("HOW TO READ THE LOG");
  Serial.println("-------------------");
  Serial.println("Every record is ONE line starting with 'REC #':");
  Serial.println("  REC #13 | t=00:14 | T1=10.2C T2=4.8C | AMB=42.0C | STATE=DOOR-OPEN | BAT=12.4V | PCM=98% HOLD=21.0h | EVENT: DOOR OPEN");
  Serial.println("  Field meaning:");
  Serial.println("    REC #  : record number - never repeats, never reorders");
  Serial.println("    t      : story clock hh:mm (compressed shipment time)");
  Serial.println("    T1/T2/T3: payload probes (T1 top-centre, T2 bottom-centre, T3 mid wall)");
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

  netTask();   // keep WiFi/MQTT alive, service subscriptions

  // ---- digital inputs (a dashboard "sim" pin wins over the Wokwi part) ----
  mainsOK  = (simMains >= 0) ? (simMains == 1) : (digitalRead(PIN_MAINS) == HIGH);
  doorOpen = (simDoor  >= 0) ? (simDoor  == 1) : (digitalRead(BTN_DOOR) == LOW);
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
  if (!isnan(simBatt)) battV = simBatt;            // scenario pin wins
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
    t3 = (probeCount > 2) ? probes.getTempCByIndex(2) : t2;
    static bool warnedDisc = false;                // one hint per failure episode, no spam
    if (t1 == DEVICE_DISCONNECTED_C && !warnedDisc) {
      warnedDisc = true;
      Serial.println("[DS18B20] !! -127 = probe did not answer (DEVICE_DISCONNECTED). "
                     "Check the DQ wires -> D4, a unique deviceID per probe, and the "
                     "4.7k pull-up (rpull) between D4 and 3V3.");
    } else if (t1 != DEVICE_DISCONNECTED_C) {
      warnedDisc = false;
    }
    // --- sensor-fault watchdog: a dead bus must NOT read as "freezing cold" ---
    // -127 would otherwise keep the hysteresis permanently satisfied and the
    // compressor parked while everything LOOKS fine. Make the failure loud.
    if (isnan(simT1) && isnan(simT2) && isnan(simT3)) {   // a dashboard pin counts as a live sensor
      bool allDead = (t1 == DEVICE_DISCONNECTED_C) && (t2 == DEVICE_DISCONNECTED_C) &&
                     (t3 == DEVICE_DISCONNECTED_C);
      faultReads = allDead ? (uint8_t)(faultReads + 1) : 0;
      bool fault = (faultReads >= 3);              // ~5 s of dead bus, not a one-off hiccup
      if (fault != sensorFault) {
        sensorFault = fault;
        if (fault)
          logEvent("SENSOR FAULT",
                   "All payload probes stopped answering (-127). The compressor is parked and the PCM holds the temperature passively while the bus is dead.");
        else
          logEvent("SENSOR RECOVERED",
                   "Payload probes answer again - compressor control resumes on the next evaluation.");
      }
    } else if (sensorFault) {
      faultReads = 0;
      sensorFault = false;
      logEvent("SENSOR RECOVERED",
               "The dashboard is pinning the payload temperature - control continues on the pinned values.");
    }
    if (!isnan(simT1)) t1 = simT1;                 // scenario pins win
    if (!isnan(simT2)) t2 = simT2;
    if (!isnan(simT3)) t3 = simT3;
  }

  // ---- DHT22 ambient every ~2 s ----
  static unsigned long lastDht = 0;
  if ((now - lastDht) >= DHT_PERIOD_MS) {
    lastDht = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) tAmb = t;
    if (!isnan(h)) hum = h;
    if (!isnan(simTamb)) tAmb = simTamb;           // scenario pin wins
  }

  control(dtStorySec);
  beepTask();

  if (now - lastOledMs >= OLED_PERIOD_MS) { lastOledMs = now; drawOled(); }
  if (now - lastLogMs  >= LOG_PERIOD_MS)  {
    lastLogMs = now;
    logRecord(nullptr, nullptr);
    publishTelemetry();   // same cadence as the serial log: every 2 s
  }
}
