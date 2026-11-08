// ember_esp32.ino — EMBER room detector (Mayssam). Board: ESP32 Dev Module.
// Libraries: DHT sensor library (Adafruit) + Adafruit Unified Sensor, ESP32Servo, ArduinoJson 7.
// Tabs: ember_esp32.ino, model.h (from train.py), ember_link.h (from Israa).
// No delay() in loop: everything runs on millis() timers. Safety logic runs on core 1,
// the network runs in its own task on core 0 (ember_link.h).
#include <Arduino.h>
#include "esp_arduino_version.h"
#include "driver/gpio.h"
#include <DHT.h>
#include <ESP32Servo.h>
#include <ArduinoJson.h>
#include "model.h"

#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

// ======================= CONFIG (edit here) =======================
#define USE_LINK        1      // 0 = board alone, no Wi-Fi (Checkpoint A)
#define HAS_LDR         0      // LDR not plugged yet -> light features = 0 + "ldr" fault. Set 1 when wired.
#define HAS_IR          1      // IR obstacle sensor fitted -> anti-masking ("detector covered")
#define BUZZER_PASSIVE  0      // 0 = active buzzer (on/off), 1 = passive buzzer (tones)
#define PIN_PULLDOWN    1      // internal pull-down on analog pins so an unplugged sensor reads ~0

#include "secrets.h"            // WIFI_PASS + EMBER_KEY (not in git: copy secrets.example.h -> secrets.h)
const char*    WIFI_SSID   = "EMBER-NET";
const char*    SERVER_IP   = "172.20.10.3";
const bool     USE_TLS     = true;
const uint16_t SERVER_PORT = USE_TLS ? 5443 : 5000;
const char*    DEV_NAME    = "room1";

// Fallback-rule thresholds: CALIBRATE on the real sensors (watch the DATA lines).
const float GAS_LEAK_TH   = 250.0f;   // gas_delta (ADC counts above baseline) -> leak
const float TEMP_SLOPE_TH = 0.40f;    // degC/s; must stay ABOVE a warm hand (trap) on the DHT11
const float FLICKER_TH    = 40.0f;    // LDR std per tick -> flame flicker
const float P_ALARM_FIRE  = 0.60f, P_WARN_FIRE = 0.30f, P_WARN_LEAK = 0.50f;

// Pins (hardware as fitted)
const int PIN_GAS = 33, PIN_DHT = 4, PIN_IR = 23, PIN_LDR = 32, PIN_BUZZER = 25;
const int PIN_LED_G = 26, PIN_LED_Y = 27, PIN_LED_R = 14, PIN_SERVO = 18, PIN_BUTTON = 0;  // BOOT button
const int SERVO_OPEN = 0, SERVO_CLOSED = 90;
const int BUZ_CH = 15;                 // LEDC channel for passive buzzer on core 2.x

// ======================= network link =======================
#if USE_LINK
#include "ember_link.h"
#else
inline void linkBegin(const char*, const char*, const char*, uint16_t, bool, const char*) {}
inline void linkPublish(const String&) {}
inline bool linkOk() { return false; }
inline float linkCv(bool* f) { if (f) *f = false; return 0; }
inline bool linkNextCommand(int32_t*, String*) { return false; }
#endif

// ======================= state =======================
#define WIN 10                         // 10 ticks x 500 ms = 5 s window
#define TICK_MS 500
enum { CAUSE_NONE = 0, CAUSE_GAS = 1, CAUSE_FIRE = 2 };
const char* LEVEL_TXT[] = {"NORMAL", "WARNING", "ALARM"};
const char* CAUSE_TXT[] = {"none", "gas_leak", "fire"};
const char* FEAT_NAME[FEAT_N] = {"gas_delta", "gas_slope", "temp_slope", "light_delta", "flicker"};

DHT dht(PIN_DHT, DHT11);
Servo valveServo;

uint32_t bootId, seqNo = 0, tickCount = 0;
float gasW[WIN], tempW[WIN], ldrW[WIN], flkW[WIN];
int wIdx = 0, wCount = 0;
inline float wAt(const float* a, int i) { return a[(wIdx + i) % WIN]; }   // i = 0 oldest .. 9 newest

// LDR fast sampling (50 Hz)
double ldrSum = 0, ldrSum2 = 0; uint32_t ldrN = 0; int ldrRawLast = 0;
// gas
float gasTick = 0, gasPrevTick = -1; int gasGood = 0, gasFlat = 0; uint32_t gasBadSince = 0; bool gasFault = false;
// dht
float tempC = NAN, humP = NAN; int dhtNan = 0, dhtGood = 0; bool dhtFault = false;
// ldr
uint32_t ldrBadSince = 0; bool ldrFault = !HAS_LDR; int ldrGood = 0; float ldrTick = 0, flkTick = 0;
// ir / tamper
uint32_t irLowSince = 0; bool tamper = false; int irRaw = 1;
// baselines
double gasBaseSum = 0, ldrBaseSum = 0; int gasBaseN = 0, ldrBaseN = 0;
float gasBase = 0, ldrBase = 0; bool baseReady = false;
// decision
float feat[FEAT_N]; bool missing[FEAT_N]; float probs[CLASS_N] = {1, 0, 0}; float why[FEAT_N];
int level = 0, cause = CAUSE_NONE, upCount = 0, calmCount = 0;
bool latched = false, prevShouldClose = false, modelMode = false;
uint32_t leakSince = 0, silenceUntil = 0;
float cvUsed = 0;
// valve
bool valveClosed = false, servoAttached = false; uint32_t servoDetachAt = 0;
// link
bool linkWasOk = false, prevLinkOk = false; int32_t lastCmd = 0;
// timers
uint32_t tLdr = 0, tTick = 0, tDht = 0, tStatus = 0;

// ======================= helpers =======================
int readAnalog(int pin) {
  int v = analogRead(pin);
#if PIN_PULLDOWN
  gpio_pulldown_en((gpio_num_t)pin);   // analogRead may reset pulls on core 2.x -> re-enable
#endif
  return v;
}

void setValve(bool closed, bool force = false) {
  if (closed == valveClosed && !force) return;
  valveClosed = closed;
  valveServo.setPeriodHertz(50);
  valveServo.attach(PIN_SERVO, 500, 2400);
  valveServo.write(closed ? SERVO_CLOSED : SERVO_OPEN);
  servoAttached = true;
  servoDetachAt = millis() + 600;       // MG996R on VIN: move, then detach (no brown-out, no jitter)
  Serial.printf("# VALVE %s\n", closed ? "CLOSED" : "OPEN");
}

void buzzerOut(int f) {                 // f = 0 off; active buzzer: any f > 0 = on
  static int cur = -1;
  if (f == cur) return;
  cur = f;
#if BUZZER_PASSIVE
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(PIN_BUZZER, f);
#else
  ledcWriteTone(BUZ_CH, f);
#endif
#else
  digitalWrite(PIN_BUZZER, f > 0 ? HIGH : LOW);
#endif
}

void ledPin(int pin) {
  pinMode(pin, OUTPUT);
  gpio_set_drive_capability((gpio_num_t)pin, GPIO_DRIVE_CAP_0);   // no series resistors fitted
  digitalWrite(pin, LOW);
}

// ======================= model =======================
void predict(const float x[FEAT_N], const bool miss[FEAT_N], float p[CLASS_N], float xs[FEAT_N]) {
  for (int j = 0; j < FEAT_N; j++) {
    xs[j] = miss[j] ? 0.0f : (x[j] - FEAT_MEAN[j]) / FEAT_SCALE[j];  // missing -> training mean
    xs[j] = constrain(xs[j], -10.0f, 10.0f);
  }
  float z[CLASS_N], zmax = -1e9f;
  for (int k = 0; k < CLASS_N; k++) {
    z[k] = B[k];
    for (int j = 0; j < FEAT_N; j++) z[k] += W[k][j] * xs[j];
    zmax = fmaxf(zmax, z[k]);
  }
  float s = 0;
  for (int k = 0; k < CLASS_N; k++) { z[k] = expf(z[k] - zmax); s += z[k]; }
  for (int k = 0; k < CLASS_N; k++) p[k] = z[k] / s;
}

void rules(const float x[FEAT_N], const bool miss[FEAT_N], float p[CLASS_N]) {
  bool flicker = !miss[4] && x[4] > FLICKER_TH;
  bool heat    = !miss[2] && x[2] > TEMP_SLOPE_TH;
  bool gasHi   = !miss[0] && x[0] > GAS_LEAK_TH;
  bool gasHalf = !miss[0] && x[0] > GAS_LEAK_TH / 2;
  float pL = 0.02f, pF = 0.02f;
  if (flicker && (heat || gasHalf)) pF = 0.90f;
  else if (miss[4] && heat && gasHalf) pF = 0.90f;   // no LDR: heat + smoke/gas = fire
  else if (gasHi) pL = 0.90f;
  else if (miss[4] && heat) pF = 0.45f;              // no LDR: heat alone = WARNING; camera can confirm to ALARM
  p[1] = pL; p[2] = pF; p[0] = 1.0f - pL - pF;
}

// ======================= level logic (contract section 3) =======================
void updateLevel(int cand, int candCause) {
  if (!latched) {
    if (cand > level) {
      calmCount = 0;
      if (++upCount >= 2) { level = cand; cause = candCause; upCount = 0; }
    } else {
      upCount = 0;
      if (cand < level) {
        if (++calmCount >= 20) { level = cand; cause = cand ? candCause : CAUSE_NONE; calmCount = 0; }
      } else calmCount = 0;
    }
    if (level == 1 && cause == CAUSE_GAS) {           // leak still WARNING after 10 s -> ALARM
      if (!leakSince) leakSince = millis();
      if (millis() - leakSince >= 10000) level = 2;
    } else leakSince = 0;
    if (level == 2) latched = true;
  }
  bool shouldClose = (level >= 1 && cause == CAUSE_GAS) || level == 2;
  if (shouldClose && !prevShouldClose) setValve(true); // close on entering; never reopens by itself
  prevShouldClose = shouldClose;
}

// ======================= sampling =======================
void sampleLdr() {
#if HAS_LDR
  int v = readAnalog(PIN_LDR);
  ldrRawLast = v;
  ldrSum += v; ldrSum2 += (double)v * v; ldrN++;
#endif
}

void readDht() {
  float t = dht.readTemperature(), h = dht.readHumidity();
  if (isnan(t) || isnan(h)) {
    if (++dhtNan >= 3 && !dhtFault) { dhtFault = true; Serial.println("# FAULT dht"); }
  } else {
    tempC = t; humP = h; dhtNan = 0;
    if (dhtFault) { dhtFault = false; dhtGood = 0; Serial.println("# dht OK"); }
  }
}

void senseTick() {
  uint32_t now = millis();
  // gas: average of 8 reads
  long s = 0;
  for (int i = 0; i < 8; i++) s += readAnalog(PIN_GAS);
  gasTick = s / 8.0f;
  bool gasBadNow = gasTick <= 20 || gasTick >= 4080;
  if (gasBadNow) { if (!gasBadSince) gasBadSince = now; } else gasBadSince = 0;
  gasFlat = (gasTick == gasPrevTick) ? gasFlat + 1 : 0;
  gasPrevTick = gasTick;
  bool gf = (gasBadSince && now - gasBadSince >= 5000) || gasFlat >= 20;
  if (gf != gasFault) { gasFault = gf; Serial.printf("# %s gas\n", gf ? "FAULT" : "OK"); }
  gasGood = (gasBadNow || gasFault) ? 0 : min(gasGood + 1, 1000);   // window must refill after a fault

  // ldr: mean + std of the 50 Hz samples of this tick
#if HAS_LDR
  if (ldrN > 0) {
    ldrTick = ldrSum / ldrN;
    flkTick = sqrt(fmax(0.0, ldrSum2 / ldrN - (double)ldrTick * ldrTick));
  }
  ldrSum = ldrSum2 = 0; ldrN = 0;
  bool ldrBadNow = ldrTick <= 5 || ldrTick >= 4090;
  if (ldrBadNow) { if (!ldrBadSince) ldrBadSince = now; } else ldrBadSince = 0;
  bool lf = ldrBadSince && now - ldrBadSince >= 10000;
  if (lf != ldrFault) { ldrFault = lf; Serial.printf("# %s ldr\n", lf ? "FAULT" : "OK"); }
  ldrGood = (ldrBadNow || ldrFault) ? 0 : min(ldrGood + 1, 1000);
#endif

  dhtGood = (dhtFault || isnan(tempC)) ? 0 : min(dhtGood + 1, 1000);

  // ir tamper
#if HAS_IR
  irRaw = digitalRead(PIN_IR);
  if (irRaw == LOW) { if (!irLowSince) irLowSince = now; } else irLowSince = 0;
  bool tp = irLowSince && now - irLowSince > 5000;
  if (tp != tamper) { tamper = tp; Serial.printf("# TAMPER %s\n", tp ? "detector covered" : "cleared"); }
#endif

  // ring buffers
  gasW[wIdx] = gasTick;
  tempW[wIdx] = isnan(tempC) ? 0 : tempC;
  ldrW[wIdx] = ldrTick;
  flkW[wIdx] = flkTick;
  wIdx = (wIdx + 1) % WIN;
  if (wCount < WIN) wCount++;

  // baselines: first 40 ticks (20 s), then slow update only while NORMAL
  tickCount++;
  if (!baseReady) {
    if (!gasBadNow) { gasBaseSum += gasTick; gasBaseN++; }
    if (HAS_LDR && !ldrFault) { ldrBaseSum += ldrTick; ldrBaseN++; }
    if (tickCount >= 40 && gasBaseN > 0) {
      gasBase = gasBaseSum / gasBaseN;
      ldrBase = ldrBaseN ? ldrBaseSum / ldrBaseN : 0;
      baseReady = true;
      Serial.printf("# baseline ready gas=%.0f ldr=%.0f\n", gasBase, ldrBase);
    }
  } else if (level == 0) {
    if (!gasFault && !gasBadNow) gasBase += 0.002f * (gasTick - gasBase);
    if (HAS_LDR && !ldrFault) ldrBase += 0.002f * (ldrTick - ldrBase);
  }
}

void computeFeatures() {
  float gm = 0, lm = 0, fm = 0;
  for (int i = 0; i < WIN; i++) { gm += gasW[i]; lm += ldrW[i]; fm += flkW[i]; }
  gm /= WIN; lm /= WIN; fm /= WIN;
  float early = (wAt(gasW, 0) + wAt(gasW, 1) + wAt(gasW, 2)) / 3;
  float late  = (wAt(gasW, 7) + wAt(gasW, 8) + wAt(gasW, 9)) / 3;
  feat[0] = gm - gasBase;
  feat[1] = (late - early) / 3.5f;
  feat[2] = (wAt(tempW, 9) - wAt(tempW, 0)) / 4.5f;
  feat[3] = HAS_LDR ? lm - ldrBase : 0;
  feat[4] = HAS_LDR ? fm : 0;
  missing[0] = missing[1] = gasFault || gasGood < WIN;
  missing[2] = dhtFault || dhtGood < WIN;
  missing[3] = missing[4] = !HAS_LDR || ldrFault || ldrGood < WIN;
  for (int j = 0; j < FEAT_N; j++) if (missing[j] || isnan(feat[j])) feat[j] = 0;   // a fault never starts an alarm
}

// ======================= decide (every tick) =======================
void decide() {
  bool ready = wCount >= WIN && baseReady;
  modelMode = ready && MODEL_READY;
  float xs[FEAT_N] = {0};
  if (!ready) { probs[0] = 1; probs[1] = probs[2] = 0; }
  else if (modelMode) predict(feat, missing, probs, xs);
  else rules(feat, missing, probs);

  // camera late fusion (camera alone max 0.5 -> WARNING, never ALARM)
  bool fresh = false;
  float cv = linkCv(&fresh);
  cvUsed = fresh ? cv : 0;
  if (ready && fresh) {
    float pf = 1 - (1 - probs[2]) * (1 - 0.5f * cv);
    probs[0] = fmaxf(0, probs[0] - (pf - probs[2]));
    probs[2] = pf;
  }

  int cand = 0, cc = CAUSE_NONE;
  if (ready) {
    if (probs[2] >= P_ALARM_FIRE)     { cand = 2; cc = CAUSE_FIRE; }
    else if (probs[1] >= P_WARN_LEAK) { cand = 1; cc = CAUSE_GAS; }
    else if (probs[2] >= P_WARN_FIRE) { cand = 1; cc = CAUSE_FIRE; }
  }
  updateLevel(cand, cc);

  // "why": contribution of each feature to the class behind the current cause (fire if none)
  int c = (cause == CAUSE_GAS) ? 1 : 2;
  if (modelMode) {
    for (int j = 0; j < FEAT_N; j++) why[j] = W[c][j] * xs[j];
  } else {
    const float ref[FEAT_N] = {GAS_LEAK_TH, GAS_LEAK_TH / 3.5f, TEMP_SLOPE_TH, 1000, FLICKER_TH};
    for (int j = 0; j < FEAT_N; j++) why[j] = missing[j] ? 0 : constrain(fabsf(feat[j]) / ref[j], 0.0f, 1.0f);
  }
}

// ======================= payload =======================
void publish() {
  JsonDocument d;
  d["dev"] = DEV_NAME; d["boot"] = bootId; d["seq"] = ++seqNo; d["up_ms"] = millis(); d["sim"] = false;
  JsonObject raw = d["raw"].to<JsonObject>();
  raw["gas"] = (int)gasTick;
  if (isnan(tempC) || dhtFault) { raw["temp"] = nullptr; raw["hum"] = nullptr; }
  else { raw["temp"] = tempC; raw["hum"] = humP; }
  raw["ldr"] = (int)ldrTick; raw["ir"] = irRaw;
  JsonObject f = d["feat"].to<JsonObject>();
  for (int j = 0; j < FEAT_N; j++) f[FEAT_NAME[j]] = roundf(feat[j] * 100) / 100;
  d["cv"] = cvUsed;
  JsonObject p = d["probs"].to<JsonObject>();
  p["normal"] = probs[0]; p["leak"] = probs[1]; p["fire"] = probs[2];
  JsonObject w = d["why"].to<JsonObject>();
  for (int j = 0; j < FEAT_N; j++) w[FEAT_NAME[j]] = roundf(why[j] * 100) / 100;
  d["level"] = level; d["state"] = LEVEL_TXT[level]; d["cause"] = CAUSE_TXT[cause];
  d["mode"] = modelMode ? "model" : "rules";
  d["latched"] = latched; d["silenced"] = millis() < silenceUntil;
  d["valve"] = valveClosed ? "closed" : "open";
  JsonArray fa = d["faults"].to<JsonArray>();
  if (gasFault) fa.add("gas");
  if (dhtFault) fa.add("dht");
  if (ldrFault) fa.add("ldr");
  d["tamper"] = tamper; d["link_ok"] = linkOk(); d["last_cmd"] = lastCmd;
  String s;
  serializeJson(d, s);
  linkPublish(s);                       // non-blocking: the link task sends it
}

void printData() {
  Serial.printf("DATA,%lu,%.0f,%.1f,%.1f,%.0f,%.2f,%.2f,%.3f,%.2f,%.2f,%d\n",
                (unsigned long)millis(), gasTick, tempC, humP, ldrTick,
                feat[0], feat[1], feat[2], feat[3], feat[4], level);
}

// ======================= commands / link / button =======================
void runCommand(int32_t id, const String& cmd) {
  Serial.printf("# CMD id=%ld %s\n", (long)id, cmd.c_str());
  if (cmd == "reset") {                 // only authorized path that clears the latch
    latched = false; level = 0; cause = CAUSE_NONE; upCount = calmCount = 0; leakSince = 0;
    prevShouldClose = false;
  } else if (cmd == "silence") silenceUntil = millis() + 60000;
  else if (cmd == "valve_open") setValve(false);
  else if (cmd == "valve_close") setValve(true);
  lastCmd = id;                         // ACK goes back in the next payload
}

void handleLink() {
  int32_t id; String cmd;
  while (linkNextCommand(&id, &cmd)) runCommand(id, cmd);
#if USE_LINK
  bool ok = linkOk();
  if (ok) linkWasOk = true;
  if (linkWasOk && prevLinkOk && !ok) {  // link lost -> safe state
    Serial.println("# LINK LOST -> deciding alone, valve closed");
    setValve(true);
  }
  prevLinkOk = ok;
#endif
}

void handleButton() {                   // BOOT button: silence 60 s, never reset
  static int last = HIGH; static uint32_t tChange = 0;
  int v = digitalRead(PIN_BUTTON);
  if (v != last && millis() - tChange > 50) {
    tChange = millis(); last = v;
    if (v == LOW) { silenceUntil = millis() + 60000; Serial.println("# SILENCE 60 s (button)"); }
  }
}

// ======================= actuators (every loop) =======================
void actuators() {
  uint32_t t = millis();
  bool sil = t < silenceUntil;
  int g = 0, y = 0, r = 0, f = 0;
  if (level == 0) {
    g = 1;
    if ((gasFault || dhtFault || (HAS_LDR && ldrFault) || tamper) && !sil && (t % 10000) < 60) f = 2500;  // fault chirp
  } else if (level == 1) {
    y = (t % 500) < 250;                              // 2 Hz blink
    if (!sil && (t % 1000) < 100) f = 2500;           // 100 ms beep every 1 s
  } else {
    r = (t % 200) < 100;                              // 5 Hz strobe
    if (!sil) {
#if BUZZER_PASSIVE
      if (cause == CAUSE_GAS) f = ((t / 400) % 2) ? 770 : 960;             // hi-lo two-tone
      else f = 600 + ((t % 1000) * 12 / 10) / 50 * 50;                     // rising sweep 600-1800 Hz
#else
      if (cause == CAUSE_GAS) f = (t % 1000) < 500;                        // slow on/off
      else f = (t % 300) < 150;                                             // fast on/off
#endif
    }
  }
  digitalWrite(PIN_LED_G, g); digitalWrite(PIN_LED_Y, y); digitalWrite(PIN_LED_R, r);
  buzzerOut(f);
  if (servoAttached && (int32_t)(t - servoDetachAt) >= 0) { valveServo.detach(); servoAttached = false; }
}

// ======================= setup / loop =======================
void setup() {
  Serial.begin(115200);
  ledPin(PIN_LED_G); ledPin(PIN_LED_Y); ledPin(PIN_LED_R);
#if BUZZER_PASSIVE
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_BUZZER, 2000, 8);
#else
  ledcSetup(BUZ_CH, 2000, 8); ledcAttachPin(PIN_BUZZER, BUZ_CH);
#endif
#else
  pinMode(PIN_BUZZER, OUTPUT); digitalWrite(PIN_BUZZER, LOW);
#endif
  buzzerOut(0);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_IR, INPUT_PULLUP);        // unplugged IR reads HIGH = not covered (no false tamper)
  analogReadResolution(12);
  readAnalog(PIN_GAS);                  // first read attaches the ADC, then pull-down is enabled
#if HAS_LDR
  readAnalog(PIN_LDR);
#endif
  dht.begin();
  setValve(false, true);                // valve open at boot

  bootId = esp_random() & 0x7fffffff;
  linkBegin(WIFI_SSID, WIFI_PASS, SERVER_IP, SERVER_PORT, USE_TLS, EMBER_KEY);
  Serial.printf("# EMBER boot=%lu core=%d model=%d ldr=%d ir=%d link=%d\n", (unsigned long)bootId,
                ESP_ARDUINO_VERSION_MAJOR, MODEL_READY, HAS_LDR, HAS_IR, USE_LINK);
  Serial.println("# baseline: keep the room calm for 20 s");
  tTick = tLdr = tDht = millis();
}

void loop() {
  uint32_t now = millis();
  if (now - tLdr >= 20) { tLdr += 20; sampleLdr(); }
  if (now - tDht >= 2000) { tDht = now; readDht(); }
  if (now - tTick >= TICK_MS) {
    tTick += TICK_MS;
    if (now - tTick > TICK_MS) tTick = now;   // don't burst after a stall
    senseTick();
    computeFeatures();
    decide();
    publish();
    printData();
  }
  handleLink();
  handleButton();
  actuators();
  if (now - tStatus >= 5000) {
    tStatus = now;
    Serial.printf("# %s cause=%s mode=%s p=[%.2f %.2f %.2f] gas=%.0f base=%.0f T=%.1f link=%s valve=%s faults=%s%s%s%s\n",
                  LEVEL_TXT[level], CAUSE_TXT[cause], modelMode ? "model" : "rules", probs[0], probs[1], probs[2],
                  gasTick, gasBase, tempC, linkOk() ? "OK" : "--", valveClosed ? "closed" : "open",
                  gasFault ? "gas " : "", dhtFault ? "dht " : "", ldrFault ? "ldr " : "", tamper ? "TAMPER" : "");
  }
}
