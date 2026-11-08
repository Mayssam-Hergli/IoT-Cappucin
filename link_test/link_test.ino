// link_test.ino — tests ember_link.h alone (dummy payload). Open Serial Monitor at 115200.
// Expect on the dashboard: live packets from dev "linktest". Click Reset/Valve on the
// dashboard (admin) -> "EXEC command" lines here and last_cmd goes up.
#include "ember_link.h"
#include "secrets.h"            // WIFI_PASS + EMBER_KEY (not in git: copy secrets.example.h -> secrets.h)

const char* WIFI_SSID = "EMBER-NET";
const char* SERVER_IP = "172.20.10.3";
const bool  USE_TLS   = true;
const uint16_t PORT   = USE_TLS ? 5443 : 5000;

uint32_t bootId, seq = 0;
String valve = "open";

void setup() {
  Serial.begin(115200);
  delay(300);
  bootId = esp_random() & 0x7fffffff;
  linkBegin(WIFI_SSID, WIFI_PASS, SERVER_IP, PORT, USE_TLS, EMBER_KEY);
  Serial.println("link_test started");
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last >= 500) {
    last = millis();
    bool fresh;
    float cv = linkCv(&fresh);

    JsonDocument d;
    d["dev"] = "linktest"; d["boot"] = bootId; d["seq"] = ++seq; d["up_ms"] = millis(); d["sim"] = false;
    JsonObject raw = d["raw"].to<JsonObject>();
    raw["gas"] = analogRead(34); raw["temp"] = 24.5; raw["hum"] = 40.0; raw["ldr"] = 2000; raw["ir"] = 1;
    JsonObject f = d["feat"].to<JsonObject>();
    f["gas_delta"] = 0; f["gas_slope"] = 0; f["temp_slope"] = 0; f["light_delta"] = 0; f["flicker"] = 0;
    d["cv"] = fresh ? cv : 0.0;
    JsonObject p = d["probs"].to<JsonObject>(); p["normal"] = 0.98; p["leak"] = 0.01; p["fire"] = 0.01;
    JsonObject w = d["why"].to<JsonObject>();
    w["gas_delta"] = 0; w["gas_slope"] = 0; w["temp_slope"] = 0; w["light_delta"] = 0; w["flicker"] = 0;
    d["level"] = 0; d["state"] = "NORMAL"; d["cause"] = "none"; d["mode"] = "rules";
    d["latched"] = false; d["silenced"] = false; d["valve"] = linkOk() ? valve : "closed";
    d["faults"].to<JsonArray>(); d["tamper"] = false; d["link_ok"] = linkOk(); d["last_cmd"] = linkLastCmd();
    String s; serializeJson(d, s);
    linkPublish(s);

    Serial.printf("seq=%lu link=%s http=%d cv=%.2f%s wifi=%d heap=%u\n", (unsigned long)seq,
                  linkOk() ? "OK" : "--", linkLastHttpCode(), cv, fresh ? "*" : "", WiFi.status(), ESP.getFreeHeap());
  }

  int32_t id; String cmd;
  while (linkNextCommand(&id, &cmd)) {
    Serial.printf(">>> EXEC command id=%ld %s\n", (long)id, cmd.c_str());
    if (cmd == "valve_open") valve = "open";
    if (cmd == "valve_close") valve = "closed";
  }
}
