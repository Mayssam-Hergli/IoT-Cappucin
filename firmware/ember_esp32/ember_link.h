// ember_link.h — EMBER network module (header-only). Owner: Israa.
// Runs Wi-Fi + HTTPS + HMAC in its own FreeRTOS task on core 0, so the safety
// loop (core 1) never waits for the network.
//
// Needs: ESP32 Arduino core, ArduinoJson 7 (Library Manager).
//
// API:
//   linkBegin(ssid, pass, host, port, tls, key);  // call once in setup()
//   linkPublish(payloadJson);                     // latest payload; the task sends it every 500 ms
//   linkOk();                                     // valid signed reply in the last 3000 ms
//   linkCv(&fresh);                               // camera score from last valid reply (0 if link lost)
//   linkNextCommand(&id, &cmd);                   // next verified command (id > last executed); marks it executed
//   linkLastCmd();                                // id of last executed command -> put in payload "last_cmd"
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "mbedtls/md.h"

#ifndef LINK_DEBUG
#define LINK_DEBUG 1          // 1 = print link status on Serial
#endif
#define LINK_PERIOD_MS 500
#define LINK_OK_MS 3000
#define LINK_QMAX 8

namespace ember_link_ns {
  static char ssid[33], pass[65], host[64], key[65];
  static uint16_t port = 5443;
  static bool tls = true;
  static SemaphoreHandle_t mtx = nullptr;
  static String payload;                 // latest payload to send
  static uint32_t lastOkMs = 0;
  static bool everOk = false;
  static float cv = 0.0f;
  static bool cvFresh = false;
  static int32_t lastExec = 0;           // last executed command id
  static int32_t qId[LINK_QMAX];
  static String qCmd[LINK_QMAX];
  static int qLen = 0;
  static int lastCode = 0;
}

// ---------------------------------------------------------------- crypto
inline String hmacHex(const String& msg) {
  using namespace ember_link_ns;
  uint8_t out[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, (const uint8_t*)key, strlen(key));
  mbedtls_md_hmac_update(&ctx, (const uint8_t*)msg.c_str(), msg.length());
  mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", out[i]);
  hex[64] = 0;
  return String(hex);
}

// constant-time compare of two 64-char hex signatures
inline bool sigEqual(const String& a, const char* b) {
  if (!b || a.length() != 64 || strlen(b) != 64) return false;
  uint8_t d = 0;
  for (int i = 0; i < 64; i++) d |= (uint8_t)a[i] ^ (uint8_t)b[i];
  return d == 0;
}

// ---------------------------------------------------------------- reply handling
inline bool linkHandleReply(const String& body) {
  using namespace ember_link_ns;
  JsonDocument env;
  if (deserializeJson(env, body)) return false;
  const char* p = env["p"];
  const char* sig = env["sig"];
  if (!p || !sig) return false;
  if (!sigEqual(hmacHex(String(p)), sig)) {          // verify BEFORE using anything
    if (LINK_DEBUG) Serial.println("[link] reply signature INVALID -> ignored");
    return false;
  }
  JsonDocument rep;
  if (deserializeJson(rep, p)) return false;
  if (!(rep["ok"] | false)) return false;

  xSemaphoreTake(mtx, portMAX_DELAY);
  lastOkMs = millis();
  everOk = true;
  cv = rep["cv"] | 0.0f;
  cvFresh = rep["cv_fresh"] | false;
  for (JsonObject c : rep["cmds"].as<JsonArray>()) {
    int32_t id = c["id"] | 0;
    const char* cmd = c["cmd"] | "";
    if (id <= lastExec) continue;                     // old / replayed command
    bool dup = false;
    for (int i = 0; i < qLen; i++) if (qId[i] == id) dup = true;
    if (dup || qLen >= LINK_QMAX) continue;
    // insert sorted by id
    int pos = qLen;
    while (pos > 0 && qId[pos - 1] > id) { qId[pos] = qId[pos - 1]; qCmd[pos] = qCmd[pos - 1]; pos--; }
    qId[pos] = id; qCmd[pos] = String(cmd); qLen++;
    if (LINK_DEBUG) Serial.printf("[link] verified command id=%ld %s\n", (long)id, cmd);
  }
  xSemaphoreGive(mtx);
  return true;
}

// ---------------------------------------------------------------- task
inline void linkTask(void*) {
  using namespace ember_link_ns;
  WiFiClientSecure sclient;
  WiFiClient pclient;
  sclient.setInsecure();               // HMAC proves who sent each message; pinning = production step
  WiFiClient& client = tls ? (WiFiClient&)sclient : pclient;
  HTTPClient http;
  http.setReuse(true);                 // keep-alive: one TLS handshake, not one per packet
  http.setTimeout(1500);
  http.setConnectTimeout(3000);
  uint32_t lastWifiTry = 0;
  TickType_t wake = xTaskGetTickCount();

  for (;;) {
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(LINK_PERIOD_MS));

    if (WiFi.status() != WL_CONNECTED) {
      if (millis() - lastWifiTry > 5000) {
        lastWifiTry = millis();
        if (LINK_DEBUG) Serial.println("[link] WiFi reconnecting...");
        WiFi.disconnect();
        WiFi.begin(ssid, pass);
      }
      continue;
    }

    String p;
    xSemaphoreTake(mtx, portMAX_DELAY);
    p = payload;
    xSemaphoreGive(mtx);
    if (p.length() == 0) continue;

    JsonDocument env;
    env["p"] = p;                      // ArduinoJson escapes the string correctly
    env["sig"] = hmacHex(p);
    String body;
    serializeJson(env, body);

    if (!http.begin(client, host, port, "/api/reading", tls)) { lastCode = -100; continue; }
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(body);
    lastCode = code;
    if (code == 200) {
      String resp = http.getString();
      http.end();                      // keeps the connection open (reuse)
      linkHandleReply(resp);
    } else {
      http.end();
      if (code < 0) client.stop();     // broken connection -> fresh one next time
      if (LINK_DEBUG) Serial.printf("[link] POST failed code=%d\n", code);
    }
  }
}

// ---------------------------------------------------------------- public API
inline void linkBegin(const char* s, const char* pw, const char* h, uint16_t prt, bool useTls, const char* k) {
  using namespace ember_link_ns;
  strlcpy(ssid, s, sizeof(ssid));
  strlcpy(pass, pw, sizeof(pass));
  strlcpy(host, h, sizeof(host));
  strlcpy(key, k, sizeof(key));
  port = prt;
  tls = useTls;
  mtx = xSemaphoreCreateMutex();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);               // lower latency
  WiFi.begin(ssid, pass);
  xTaskCreatePinnedToCore(linkTask, "ember_link", 16384, nullptr, 1, nullptr, 0);
}

inline void linkPublish(const String& payloadJson) {
  using namespace ember_link_ns;
  if (!mtx) return;
  xSemaphoreTake(mtx, portMAX_DELAY);
  payload = payloadJson;
  xSemaphoreGive(mtx);
}

inline bool linkOk() {
  using namespace ember_link_ns;
  if (!mtx) return false;
  xSemaphoreTake(mtx, portMAX_DELAY);
  bool ok = everOk && (millis() - lastOkMs < LINK_OK_MS);
  xSemaphoreGive(mtx);
  return ok;
}

inline float linkCv(bool* fresh) {
  using namespace ember_link_ns;
  bool ok = linkOk();
  xSemaphoreTake(mtx, portMAX_DELAY);
  float v = ok ? cv : 0.0f;
  bool f = ok && cvFresh;
  xSemaphoreGive(mtx);
  if (fresh) *fresh = f;
  return v;
}

inline bool linkNextCommand(int32_t* id, String* cmd) {
  using namespace ember_link_ns;
  if (!mtx) return false;
  bool got = false;
  xSemaphoreTake(mtx, portMAX_DELAY);
  while (qLen > 0) {
    int32_t i0 = qId[0];
    String c0 = qCmd[0];
    for (int i = 1; i < qLen; i++) { qId[i - 1] = qId[i]; qCmd[i - 1] = qCmd[i]; }
    qLen--;
    if (i0 > lastExec) {               // ids only go up
      lastExec = i0;
      if (id) *id = i0;
      if (cmd) *cmd = c0;
      got = true;
      break;
    }
  }
  xSemaphoreGive(mtx);
  return got;
}

inline int32_t linkLastCmd() {
  using namespace ember_link_ns;
  if (!mtx) return 0;
  xSemaphoreTake(mtx, portMAX_DELAY);
  int32_t v = lastExec;
  xSemaphoreGive(mtx);
  return v;
}

inline int linkLastHttpCode() { return ember_link_ns::lastCode; }
