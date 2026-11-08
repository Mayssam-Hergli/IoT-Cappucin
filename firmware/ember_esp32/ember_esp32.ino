// EMBER — early fire & gas-leak detector (IoT Spark 4.0, team Cappucin)
// ESP32 DevKit V1. One 500 ms tick: sample -> 5 features -> model/rules -> fusion -> level -> actuators -> payload.
// No delay() in loop. See firmware/CLAUDE.md for the spec and HARDWARE AS FITTED.
#include <Arduino.h>
#include <DHT.h>
#include <ESP32Servo.h>
#include <ArduinoJson.h>
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_random.h"
#include "model.h"

// ---------------- build switches ----------------
#define USE_LINK        0     // 1 when Israa's ember_link.h + secrets.h are in this folder
#define HAS_LDR         0     // LDR not fitted -> light_delta = flicker = 0
#define HAS_IR          0     // IR obstacle = anti-masking (tamper). Removed for now.
#define HAS_DHT         0     // DHT11 not fitted for now -> temp_slope = 0, no "dht" fault
#define BUZZER_PASSIVE  0     // hw_test: sounds on tone AND on DC -> ACTIVE buzzer
#define FORCE_RULES     0     // 1 = ignore model.h and use the fallback rules
#define SERIAL_CMDS     1     // dev only: type reset/silence/valve_open/valve_close on Serial. Set 0 for the demo.
#define PRINT_JSON      0     // 1 = print the payload JSON on Serial (debug)
#define DHT_TYPE        DHT11

// ---------------- thresholds (calibrate on the real sensors) ----------------
float GAS_LEAK_TH   = 300.0f;  // gas_delta (ADC counts)
float TEMP_SLOPE_TH = 0.40f;   // °C/s over the 4.5 s window
float FLICKER_TH    = 20.0f;   // LDR std (only with HAS_LDR)

// ---------------- pins ----------------
#define PIN_GAS    33
#define PIN_DHT     4
#define PIN_IR     23
#define PIN_LDR    32
#define PIN_BOOT    0
#define PIN_LED_G  26
#define PIN_LED_Y  27
#define PIN_LED_R  14
#define PIN_BUZ    25
#define PIN_SERVO  18
#define BUZ_CH      6          // LEDC channel 6 -> timer 3; servo gets timer 0

#if USE_LINK
#include "secrets.h"           // WIFI_SSID, WIFI_PASS, SERVER_IP, EMBER_KEY, USE_TLS
#include "ember_link.h"
#endif

// ---------------- constants ----------------
#define TICK_MS      500
#define WIN          10        // 10 ticks = 5 s
#define BASE_TICKS   40        // 20 s baseline
#define BASE_ALPHA   0.002f
#define SILENCE_MS   60000UL
#define SERVO_HOLD   600       // ms held after reaching the target, then detach
#define SERVO_STEP   3         // degrees per step: soft move limits the current spike on USB power
#define SERVO_STEP_MS 20
#define ANG_OPEN     0
#define ANG_CLOSED   90

enum Level { NORMAL = 0, WARNING = 1, ALARM = 2 };
enum Cause { C_NONE = 0, C_LEAK = 1, C_FIRE = 2 };
const char* LEVEL_TXT[3] = {"NORMAL", "WARNING", "ALARM"};
const char* CAUSE_TXT[3] = {"none", "gas_leak", "fire"};
const char* FEAT_NAME[FEAT_N] = {"gas_delta", "gas_slope", "temp_slope", "light_delta", "flicker"};
enum { F_GAS_DELTA = 0, F_GAS_SLOPE, F_TEMP_SLOPE, F_LIGHT_DELTA, F_FLICKER };

DHT dht(PIN_DHT, DHT_TYPE);
Servo valveServo;

// ---------------- state ----------------
uint32_t bootId, seq = 0, tickN = 0;
uint32_t tTick = 0, tDht = 0, tLdr = 0;

// raw
int   gasTick = 0;
float tempHeld = NAN, humHeld = NAN;
int   dhtNan = 0; bool dhtNeedRefill = true;
int   ldrRaw = 0; float ldrSum = 0, ldrSum2 = 0; int ldrN = 0;
float ldrMeanTick = 0, flickerTick = 0;
int   irRaw = 1;

// windows (index 0 = oldest)
float rGas[WIN], rTemp[WIN], rLdr[WIN], rFlk[WIN];
float gasBase = 0, ldrBase = 0; double gasBaseSum = 0, ldrBaseSum = 0;
int   gasBaseN = 0; bool gasBaseReady = false;
bool  featReady = false;

// features / model
float feat[FEAT_N] = {0}, xs[FEAT_N] = {0}, why[FEAT_N] = {0};
bool  missing[FEAT_N] = {false};
float pNormal = 1, pLeak = 0, pFire = 0, pFireSensor = 0, cv = 0;
bool  cvFresh = false, modeModel = false;

// faults / tamper
bool fGas = false, fDht = false, fLdr = false, tamper = false;
int  gasBadTicks = 0, gasFlatTicks = 0, ldrBadTicks = 0; int gasPrev = -1;
uint32_t irLowSince = 0, tChirp = 0;

// levels
Level level = NORMAL; Cause cause = C_NONE;
bool  latched = false;
int   upTicks = 0, calmTicks = 0, leakWarnTicks = 0;
uint32_t silencedUntil = 0;
bool  valveClosed = false; int servoTarget = ANG_OPEN, servoPos = ANG_OPEN; uint32_t servoT0 = 0; bool servoAttached = false;
bool  linkWasOk = false, linkLost = false;
int32_t lastCmd = 0;

// ---------------- helpers ----------------
inline bool silenced() { return (int32_t)(silencedUntil - millis()) > 0; }
float wmean(const float* r, int a, int b) { float s = 0; for (int i = a; i <= b; i++) s += r[i]; return s / (b - a + 1); }
void push(float* r, float v) { memmove(r, r + 1, sizeof(float) * (WIN - 1)); r[WIN - 1] = v; }
void fill(float* r, float v) { for (int i = 0; i < WIN; i++) r[i] = v; }
float r2(float v) { return roundf(v * 100.0f) / 100.0f; }

// ---------------- buzzer ----------------
uint32_t buzF = 0xFFFFFFFF;
void buzInit() {
  pinMode(PIN_BUZ, OUTPUT); digitalWrite(PIN_BUZ, LOW);
#if BUZZER_PASSIVE
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttachChannel(PIN_BUZ, 2000, 8, BUZ_CH);
#else
  ledcSetup(BUZ_CH, 2000, 8); ledcAttachPin(PIN_BUZ, BUZ_CH);
#endif
#endif
  gpio_set_drive_capability((gpio_num_t)PIN_BUZ, GPIO_DRIVE_CAP_1);
}
void buz(uint32_t f) {               // 0 = off. Active buzzer: any f > 0 = on.
  if (f == buzF) return;
  buzF = f;
#if BUZZER_PASSIVE
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(PIN_BUZ, f);
#else
  ledcWriteTone(BUZ_CH, f);
#endif
#else
  digitalWrite(PIN_BUZ, f ? HIGH : LOW);
#endif
}

// ---------------- servo (attach -> write -> detach after 600 ms) ----------------
void setValve(bool closed, const char* why_) {
  if (closed == valveClosed) return;
  valveClosed = closed;
  servoTarget = closed ? ANG_CLOSED : ANG_OPEN;
  if (!servoAttached) { valveServo.attach(PIN_SERVO, 500, 2400); valveServo.write(servoPos); servoAttached = true; }
  servoT0 = millis();
  Serial.printf("# EVT valve %s (%s)\n", closed ? "CLOSED" : "OPEN", why_);
}
void servoService() {          // non-blocking soft move, then detach (no holding current)
  if (!servoAttached) return;
  uint32_t now = millis();
  if (servoPos != servoTarget) {
    if (now - servoT0 >= SERVO_STEP_MS) {
      servoT0 = now;
      int d = servoTarget - servoPos;
      servoPos += constrain(d, -SERVO_STEP, SERVO_STEP);
      valveServo.write(servoPos);
    }
  } else if (now - servoT0 >= SERVO_HOLD) { valveServo.detach(); servoAttached = false; }
}

// ---------------- commands ----------------
void doReset() {
  level = NORMAL; cause = C_NONE; latched = false;
  upTicks = calmTicks = leakWarnTicks = 0;
  Serial.println("# EVT reset -> latch cleared");
}
void execCmd(const String& c) {
  if (c == "reset") doReset();
  else if (c == "silence") { silencedUntil = millis() + SILENCE_MS; Serial.println("# EVT silenced 60 s (cmd)"); }
  else if (c == "valve_open") setValve(false, "cmd");
  else if (c == "valve_close") setValve(true, "cmd");
  else Serial.printf("# EVT unknown cmd '%s'\n", c.c_str());
}

// ---------------- model ----------------
void predict(const float x[FEAT_N], const bool miss[FEAT_N], float probs[CLASS_N], float xsOut[FEAT_N]) {
  for (int j = 0; j < FEAT_N; j++) {
    xsOut[j] = miss[j] ? 0.0f : (x[j] - FEAT_MEAN[j]) / FEAT_SCALE[j];
    xsOut[j] = constrain(xsOut[j], -10.0f, 10.0f);
  }
  float z[CLASS_N], zmax = -1e9;
  for (int k = 0; k < CLASS_N; k++) {
    z[k] = B[k];
    for (int j = 0; j < FEAT_N; j++) z[k] += W[k][j] * xsOut[j];
    zmax = max(zmax, z[k]);
  }
  float s = 0;
  for (int k = 0; k < CLASS_N; k++) { z[k] = expf(z[k] - zmax); s += z[k]; }
  for (int k = 0; k < CLASS_N; k++) probs[k] = z[k] / s;
}

void rules() {
  bool fire = false, leak = false;
  bool tempOk = HAS_DHT && !missing[F_TEMP_SLOPE], gasOk = !missing[F_GAS_DELTA];
#if HAS_LDR
  fire = !missing[F_FLICKER] && feat[F_FLICKER] > FLICKER_TH &&
         ((tempOk && feat[F_TEMP_SLOPE] > TEMP_SLOPE_TH) || (gasOk && feat[F_GAS_DELTA] > GAS_LEAK_TH / 2));
#else
  fire = tempOk && feat[F_TEMP_SLOPE] > TEMP_SLOPE_TH;   // no LDR: fast heat rise only
#endif
  leak = gasOk && feat[F_GAS_DELTA] > GAS_LEAK_TH;
  if (fire)      { pNormal = 0.05f; pLeak = 0.05f; pFireSensor = 0.90f; }
  else if (leak) { pNormal = 0.05f; pLeak = 0.90f; pFireSensor = 0.05f; }
  else           { pNormal = 0.98f; pLeak = 0.01f; pFireSensor = 0.01f; }
}

// ---------------- sampling ----------------
void sampleLdrFast() {
#if HAS_LDR
  int v = analogRead(PIN_LDR); ldrRaw = v;
  ldrSum += v; ldrSum2 += (float)v * v; ldrN++;
#endif
}

void readDht() {
  float t = dht.readTemperature(), h = dht.readHumidity();
  if (isnan(t) || isnan(h)) {
    if (++dhtNan >= 3 && !fDht) { fDht = true; Serial.println("# EVT FAULT dht"); }
    if (dhtNan >= 1) dhtNeedRefill = true;   // after a gap, restart the temp window (no fake slope)
    return;
  }
  dhtNan = 0;
  if (fDht) { fDht = false; Serial.println("# EVT dht ok"); }
  tempHeld = t; humHeld = h;
  if (dhtNeedRefill) { fill(rTemp, t); dhtNeedRefill = false; }
}

void updateFaults() {
  // gas: out of range for 5 s, or perfectly flat for 10 s
  if (gasTick <= 20 || gasTick >= 4080) gasBadTicks++; else gasBadTicks = 0;
  if (gasTick == gasPrev) gasFlatTicks++; else gasFlatTicks = 0;
  gasPrev = gasTick;
  bool g = gasBadTicks >= 10 || gasFlatTicks >= 20;
  if (g && !fGas) Serial.println("# EVT FAULT gas");
  if (!g && fGas) {             // sensor back: restart window + re-learn baseline (no false alarm on replug)
    Serial.println("# EVT gas ok -> re-learning baseline 20 s");
    fill(rGas, gasTick); gasBaseReady = false; gasBaseN = 0; gasBaseSum = 0;
  }
  fGas = g;
#if HAS_LDR
  if (ldrRaw <= 5 || ldrRaw >= 4090) ldrBadTicks++; else ldrBadTicks = 0;
  bool l = ldrBadTicks >= 20;
  if (l && !fLdr) Serial.println("# EVT FAULT ldr");
  if (!l && fLdr) { fill(rLdr, ldrMeanTick); }
  fLdr = l;
#endif
#if HAS_IR
  irRaw = digitalRead(PIN_IR);
  if (irRaw == LOW) { if (!irLowSince) irLowSince = millis() | 1; }
  else irLowSince = 0;
  bool tp = irLowSince && millis() - irLowSince > 5000;
  if (tp != tamper) Serial.printf("# EVT tamper %s\n", tp ? "ON (detector covered)" : "off");
  tamper = tp;
#endif
}

void computeFeatures() {
  feat[F_GAS_DELTA]  = wmean(rGas, 0, WIN - 1) - gasBase;
  feat[F_GAS_SLOPE]  = (wmean(rGas, 7, 9) - wmean(rGas, 0, 2)) / 3.5f;
  feat[F_TEMP_SLOPE] = (rTemp[WIN - 1] - rTemp[0]) / 4.5f;
#if HAS_LDR
  feat[F_LIGHT_DELTA] = wmean(rLdr, 0, WIN - 1) - ldrBase;
  feat[F_FLICKER]     = wmean(rFlk, 0, WIN - 1);
#else
  feat[F_LIGHT_DELTA] = 0; feat[F_FLICKER] = 0;
#endif
  if (isnan(feat[F_TEMP_SLOPE]) || !HAS_DHT) feat[F_TEMP_SLOPE] = 0;
  missing[F_GAS_DELTA] = missing[F_GAS_SLOPE] = fGas || !gasBaseReady;
  if (!gasBaseReady) feat[F_GAS_DELTA] = feat[F_GAS_SLOPE] = 0;
  missing[F_TEMP_SLOPE] = HAS_DHT && (fDht || isnan(tempHeld));   // not fitted -> constant 0, not a fault
  missing[F_LIGHT_DELTA] = missing[F_FLICKER] = fLdr;
}

// ---------------- level logic (shared contract §3) ----------------
void decide() {
  Level cand = NORMAL; Cause cc = C_NONE;
  if (pFire >= 0.6f)      { cand = ALARM;   cc = C_FIRE; }
  else if (pLeak >= 0.5f) { cand = WARNING; cc = C_LEAK; }
  else if (pFire >= 0.3f) { cand = WARNING; cc = C_FIRE; }
  if (!featReady) { cand = NORMAL; cc = C_NONE; }

  Level prev = level;
  if (cand > level) {
    if (++upTicks >= 2) { level = cand; cause = cc; upTicks = 0; }
  } else upTicks = 0;

  if (level == WARNING) {
    if (cand >= WARNING) { calmTicks = 0; if (cc == C_FIRE) cause = C_FIRE; }
    else if (++calmTicks >= 20) { level = NORMAL; cause = C_NONE; calmTicks = 0; }
    // gas leak that stays WARNING 10 s -> ALARM
    if (level == WARNING && cause == C_LEAK && cand >= WARNING) {
      if (++leakWarnTicks >= 20) { level = ALARM; cause = C_LEAK; }
    }
  }
  if (level != WARNING) leakWarnTicks = 0;
  if (level == ALARM) { latched = true; if (cand == ALARM && cc == C_FIRE) cause = C_FIRE; }
  if (level == NORMAL) calmTicks = 0;

  if (level != prev) Serial.printf("# EVT level %s -> %s (cause %s)\n", LEVEL_TXT[prev], LEVEL_TXT[level], CAUSE_TXT[cause]);

  // valve: closes on gas WARNING, any ALARM, link lost. Never reopens by itself.
  if (level == ALARM) setValve(true, "alarm");
  else if (level == WARNING && cause == C_LEAK) setValve(true, "gas warning");
  if (linkLost) setValve(true, "link lost");
}

// ---------------- LEDs + buzzer patterns (called every loop) ----------------
void actuators() {
  uint32_t t = millis();
  bool g = false, y = false, r = false; uint32_t f = 0;
  if (level == NORMAL) g = true;
  else if (level == WARNING) {
    y = (t % 500) < 250;                         // 2 Hz
    if ((t % 1000) < 100) f = 2000;              // 100 ms beep / s
  } else {
    r = (t % 200) < 100;                         // 5 Hz strobe
#if BUZZER_PASSIVE
    if (cause == C_LEAK) f = (t % 500) < 250 ? 1400 : 700;          // hi-lo
    else f = 600 + (t % 1000) * 2;                                  // rising sweep 600..2600 Hz
#else
    if (cause == C_LEAK) f = (t % 1000) < 500 ? 1 : 0;              // slow
    else f = (t % 200) < 100 ? 1 : 0;                               // fast
#endif
  }
  if ((fGas || fDht || fLdr) && level == NORMAL && t - tChirp >= 10000) tChirp = t;
  if (level == NORMAL && t - tChirp < 40 && (fGas || fDht || fLdr)) f = 3000;   // fault chirp
  if (silenced()) f = 0;
  digitalWrite(PIN_LED_G, g); digitalWrite(PIN_LED_Y, y); digitalWrite(PIN_LED_R, r);
  buz(f);
}

// ---------------- payload (shared contract §1) ----------------
String buildPayload() {
  JsonDocument d;
  d["dev"] = "room1"; d["boot"] = bootId; d["seq"] = ++seq; d["up_ms"] = millis(); d["sim"] = false;
  JsonObject raw = d["raw"].to<JsonObject>();
  raw["gas"] = gasTick;
  if (isnan(tempHeld)) raw["temp"] = nullptr; else raw["temp"] = r2(tempHeld);
  if (isnan(humHeld))  raw["hum"]  = nullptr; else raw["hum"]  = r2(humHeld);
  raw["ldr"] = HAS_LDR ? ldrRaw : 0;
  raw["ir"] = irRaw;
  JsonObject fo = d["feat"].to<JsonObject>();
  for (int j = 0; j < FEAT_N; j++) fo[FEAT_NAME[j]] = r2(feat[j]);
  d["cv"] = r2(cv);
  JsonObject pr = d["probs"].to<JsonObject>();
  pr["normal"] = r2(pNormal); pr["leak"] = r2(pLeak); pr["fire"] = r2(pFire);
  JsonObject wy = d["why"].to<JsonObject>();
  for (int j = 0; j < FEAT_N; j++) wy[FEAT_NAME[j]] = r2(why[j]);
  d["level"] = (int)level; d["state"] = LEVEL_TXT[level]; d["cause"] = CAUSE_TXT[cause];
  d["mode"] = modeModel ? "model" : "rules";
  d["latched"] = latched; d["silenced"] = silenced(); d["valve"] = valveClosed ? "closed" : "open";
  JsonArray fa = d["faults"].to<JsonArray>();
  if (fGas) fa.add("gas"); if (fDht) fa.add("dht"); if (fLdr) fa.add("ldr");
  d["tamper"] = tamper; d["link_ok"] = !linkLost && linkWasOk; d["last_cmd"] = lastCmd;
  String s; serializeJson(d, s); return s;
}

// ---------------- the 500 ms tick ----------------
void tick() {
  tickN++;
  // gas: average of 8
  uint32_t s = 0; for (int i = 0; i < 8; i++) s += analogRead(PIN_GAS);
  gasTick = s / 8;
#if HAS_LDR
  ldrMeanTick = ldrN ? ldrSum / ldrN : 0;
  flickerTick = ldrN ? sqrtf(max(0.0f, ldrSum2 / ldrN - ldrMeanTick * ldrMeanTick)) : 0;
  ldrSum = ldrSum2 = 0; ldrN = 0;
#endif
  updateFaults();

  if (tickN == 1) { fill(rGas, gasTick); fill(rLdr, ldrMeanTick); fill(rFlk, flickerTick); }
  push(rGas, gasTick); push(rLdr, ldrMeanTick); push(rFlk, flickerTick);
  push(rTemp, isnan(tempHeld) ? 0 : tempHeld);

  // gas baseline: mean of the first 40 GOOD ticks (re-learnt after a gas fault), then slow update only while NORMAL
  bool gasGood = !fGas && gasTick > 20 && gasTick < 4080;
  if (!gasBaseReady) {
    if (gasGood) { gasBaseSum += gasTick;
      if (++gasBaseN >= BASE_TICKS) { gasBase = gasBaseSum / BASE_TICKS; gasBaseReady = true;
        Serial.printf("# EVT baseline gas=%.1f\n", gasBase); } }
  } else if (level == NORMAL && gasGood) gasBase += BASE_ALPHA * (gasTick - gasBase);
  // ldr baseline: first 40 ticks
  if (tickN <= BASE_TICKS) {
    ldrBaseSum += ldrMeanTick;
    if (tickN == BASE_TICKS) ldrBase = ldrBaseSum / BASE_TICKS;
  } else if (level == NORMAL && !fLdr) ldrBase += BASE_ALPHA * (ldrMeanTick - ldrBase);
  featReady = tickN >= BASE_TICKS && tickN >= WIN;

  computeFeatures();

  // model or rules
  modeModel = MODEL_READY && !FORCE_RULES && featReady;
  for (int j = 0; j < FEAT_N; j++) why[j] = 0;
  if (modeModel) {
    float p[CLASS_N]; predict(feat, missing, p, xs);
    pNormal = p[0]; pLeak = p[1]; pFireSensor = p[2];
  } else if (featReady) rules();
  else { pNormal = 1; pLeak = 0; pFireSensor = 0; }

  // link + camera fusion
#if USE_LINK
  bool ok = linkOk();
  if (ok) linkWasOk = true;
  linkLost = linkWasOk && !ok;
  cv = ok ? linkCv(&cvFresh) : 0; if (!ok) cvFresh = false;
#else
  cv = 0; cvFresh = false;
#endif
  pFire = cvFresh ? 1.0f - (1.0f - pFireSensor) * (1.0f - 0.5f * cv) : pFireSensor;

  decide();

  if (modeModel) {
    int c = cause == C_LEAK ? 1 : 2;     // class behind the cause (fire if none)
    for (int j = 0; j < FEAT_N; j++) why[j] = W[c][j] * xs[j];
  }

#if USE_LINK || PRINT_JSON
  String payload = buildPayload();
#if USE_LINK
  linkPublish(payload);
  int32_t id; String cmd;
  while (linkNextCommand(&id, &cmd)) { Serial.printf("# EVT cmd %ld %s\n", (long)id, cmd.c_str()); execCmd(cmd); lastCmd = id; }
#endif
#if PRINT_JSON
  Serial.println(payload);
#endif
#endif

  if (featReady) {
    Serial.printf("DATA,%lu,%d,%.1f,%.1f,%d,%.2f,%.2f,%.3f,%.2f,%.2f,%d\n",
      (unsigned long)millis(), gasTick, isnan(tempHeld) ? -1.0f : tempHeld, isnan(humHeld) ? -1.0f : humHeld,
      HAS_LDR ? ldrRaw : 0, feat[0], feat[1], feat[2], feat[3], feat[4], (int)level);
  } else if (tickN % 4 == 0) {
    Serial.printf("# warming up %lu/%d gas=%d temp=%.1f\n", (unsigned long)tickN, BASE_TICKS, gasTick, tempHeld);
  }
  Serial.printf("ST,%s,%s,pn=%.2f,pl=%.2f,pf=%.2f,%s,flt=%s%s%s,tamper=%d,sil=%d,valve=%s,link=%d\n",
    LEVEL_TXT[level], CAUSE_TXT[cause], pNormal, pLeak, pFire, modeModel ? "model" : "rules",
    fGas ? "gas;" : "", fDht ? "dht;" : "", fLdr ? "ldr;" : "", tamper, silenced(),
    valveClosed ? "closed" : "open", !linkLost);
}

// ---------------- setup / loop ----------------
String serBuf;
bool bootPrev = false; uint32_t tBootChg = 0;

void setup() {
  Serial.begin(115200);
  bootId = esp_random() & 0x7FFFFFFF;
  const int leds[3] = {PIN_LED_G, PIN_LED_Y, PIN_LED_R};
  for (int i = 0; i < 3; i++) {
    pinMode(leds[i], OUTPUT); digitalWrite(leds[i], LOW);
    gpio_set_drive_capability((gpio_num_t)leds[i], GPIO_DRIVE_CAP_0);
  }
  pinMode(PIN_BOOT, INPUT_PULLUP);
#if HAS_IR
  pinMode(PIN_IR, INPUT);
#endif
  analogReadResolution(12);
  analogRead(PIN_GAS);
  rtc_gpio_pullup_dis(GPIO_NUM_33);
  rtc_gpio_pulldown_en(GPIO_NUM_33);
#if HAS_LDR
  analogRead(PIN_LDR);
  rtc_gpio_pullup_dis(GPIO_NUM_32);
  rtc_gpio_pulldown_en(GPIO_NUM_32);
#endif
  ESP32PWM::allocateTimer(0);
  valveServo.setPeriodHertz(50);
  buzInit();
  dht.begin();
  // valve assumed open at boot (no move: avoids a current spike on USB power)
#if USE_LINK
  linkBegin(WIFI_SSID, WIFI_PASS, SERVER_IP, USE_TLS ? 5443 : 5000, USE_TLS, EMBER_KEY);
#endif
  Serial.printf("# EMBER boot=%lu model_ready=%d gas_th=%.0f temp_th=%.2f passive=%d link=%d dht=%d ir=%d ldr=%d\n",
    (unsigned long)bootId, MODEL_READY, GAS_LEAK_TH, TEMP_SLOPE_TH, BUZZER_PASSIVE, USE_LINK, HAS_DHT, HAS_IR, HAS_LDR);
  tTick = millis(); tDht = millis() - 2000;
}

void loop() {
  uint32_t now = millis();
#if HAS_LDR
  if (now - tLdr >= 20) { tLdr = now; sampleLdrFast(); }
#endif
#if HAS_DHT
  if (now - tDht >= 2000) { tDht = now; readDht(); }
#endif
  if (now - tTick >= TICK_MS) { tTick = now; tick(); }

  // BOOT button = silence 60 s (never reset)
  bool b = digitalRead(PIN_BOOT) == LOW;
  if (b != bootPrev && now - tBootChg > 40) {
    bootPrev = b; tBootChg = now;
    if (b) { silencedUntil = now + SILENCE_MS; Serial.println("# EVT silenced 60 s (BOOT button)"); }
  }

#if SERIAL_CMDS
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') { serBuf.trim(); if (serBuf.length()) execCmd(serBuf); serBuf = ""; }
    else if (serBuf.length() < 32) serBuf += c;
  }
#endif

  actuators();
  servoService();
}
