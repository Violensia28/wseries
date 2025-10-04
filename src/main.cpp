
#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include "config.h"

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
Preferences prefs;

struct RuntimeCfg {
  String backend; // "AC" | "DC" | "Supercap"
  struct { bool zero_cross; bool half_cycle; } ac;
  struct { float i_limit; uint8_t pwm; } dc;
  struct { bool precharge; float joule; } sc;
  struct { bool zmpt, acs712, ina219, ads1115, web_beep; } sensors;
  struct { float v_cutoff, i_guard; bool mcb_guard; } guards;
  struct { bool enabled; float i_thresh_arms; float v_cutin_vrms; uint16_t settle_ms; uint16_t retrig_ms; } aut; // AC only for W0W1
  uint8_t active_slot;
} cfg;

// ----- Helpers -----
static String macSuffix() {
  uint8_t m[6]; WiFi.macAddress(m);
  char buf[16]; snprintf(buf, sizeof(buf), "%02X%02X", m[4], m[5]);
  return String(buf);
}

static void loadDefaults() {
  cfg.backend = "AC";
  cfg.ac = { true, false };
  cfg.dc = { 15.0f, 80 };
  cfg.sc = { false, 0.0f };
  cfg.sensors = { true, true, false, false, true };
  cfg.guards = { 180.0f, 15.0f, true };
  cfg.aut = { true, 1.5f, 180.0f, 60, 800 };
  cfg.active_slot = 1;
}

static void loadConfig() {
  prefs.begin("wseries", true);
  String j = prefs.getString("runtime", "");
  prefs.end();
  if (j.length() == 0) { loadDefaults(); return; }
  DynamicJsonDocument d(2048);
  DeserializationError e = deserializeJson(d, j);
  if (e) { loadDefaults(); return; }
  cfg.backend = d["backend"].as<String>();
  cfg.ac.zero_cross = d["ac"]["zero_cross"].as<bool>();
  cfg.ac.half_cycle = d["ac"]["half_cycle"].as<bool>();
  cfg.dc.i_limit = d["dc"]["i_limit"].as<float>();
  cfg.dc.pwm = d["dc"]["pwm"].as<uint8_t>();
  cfg.sc.precharge = d["sc"]["precharge"].as<bool>();
  cfg.sc.joule = d["sc"]["joule"].as<float>();
  cfg.sensors.zmpt = d["sensors"]["zmpt"].as<bool>();
  cfg.sensors.acs712 = d["sensors"]["acs712"].as<bool>();
  cfg.sensors.ina219 = d["sensors"]["ina219"].as<bool>();
  cfg.sensors.ads1115 = d["sensors"]["ads1115"].as<bool>();
  cfg.sensors.web_beep = d["sensors"]["web_beep"].as<bool>();
  cfg.guards.v_cutoff = d["guards"]["v_cutoff"].as<float>();
  cfg.guards.i_guard = d["guards"]["i_guard"].as<float>();
  cfg.guards.mcb_guard = d["guards"]["mcb_guard"].as<bool>();
  if (d["aut"].is<JsonObject>()) {
    cfg.aut.enabled = d["aut"]["enabled"].as<bool>();
    cfg.aut.i_thresh_arms = d["aut"]["i_thresh_arms"].as<float>();
    cfg.aut.v_cutin_vrms = d["aut"]["v_cutin_vrms"].as<float>();
    cfg.aut.settle_ms = d["aut"]["settle_ms"].as<uint16_t>();
    cfg.aut.retrig_ms = d["aut"]["retrig_ms"].as<uint16_t>();
  }
  cfg.active_slot = d["slots"]["active"].as<uint8_t>();
}

static void saveConfig() {
  DynamicJsonDocument d(2048);
  d["backend"] = cfg.backend;
  JsonObject ac = d.createNestedObject("ac"); ac["zero_cross"] = cfg.ac.zero_cross; ac["half_cycle"] = cfg.ac.half_cycle;
  JsonObject dc = d.createNestedObject("dc"); dc["i_limit"] = cfg.dc.i_limit; dc["pwm"] = cfg.dc.pwm;
  JsonObject sc = d.createNestedObject("sc"); sc["precharge"] = cfg.sc.precharge; sc["joule"] = cfg.sc.joule;
  JsonObject sensors = d.createNestedObject("sensors");
  sensors["zmpt"] = cfg.sensors.zmpt; sensors["acs712"] = cfg.sensors.acs712;
  sensors["ina219"] = cfg.sensors.ina219; sensors["ads1115"] = cfg.sensors.ads1115; sensors["web_beep"] = cfg.sensors.web_beep;
  JsonObject guards = d.createNestedObject("guards");
  guards["v_cutoff"] = cfg.guards.v_cutoff; guards["i_guard"] = cfg.guards.i_guard; guards["mcb_guard"] = cfg.guards.mcb_guard;
  JsonObject aut = d.createNestedObject("aut");
  aut["enabled"] = cfg.aut.enabled; aut["i_thresh_arms"] = cfg.aut.i_thresh_arms; aut["v_cutin_vrms"] = cfg.aut.v_cutin_vrms;
  aut["settle_ms"] = cfg.aut.settle_ms; aut["retrig_ms"] = cfg.aut.retrig_ms;
  JsonObject slots = d.createNestedObject("slots"); slots["active"] = cfg.active_slot;
  String j; serializeJson(d, j);
  prefs.begin("wseries", false); prefs.putString("runtime", j); prefs.end();
}

// ----- HTTP Handlers -----
static void setupRoutes() {
  server.on("/api/capabilities", HTTP_GET, [](AsyncWebServerRequest* req){
    DynamicJsonDocument d(1024);
    d["fw"] = FW_ID;
    JsonArray backs = d.createNestedArray("backends"); backs.add("AC"); backs.add("DC"); backs.add("Supercap");
    JsonObject sp = d.createNestedObject("sensors");
    // NOTE: For W0W1 we don't probe, just advertise toggles; next sprint will fill present flags
    sp["zmpt_present"] = true; sp["acs_present"] = true; sp["ina_present"] = false; sp["ads_present"] = false;
    String j; serializeJson(d, j); req->send(200, "application/json", j);
  });

  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* req){
    DynamicJsonDocument d(2048);
    d["backend"] = cfg.backend;
    JsonObject ac = d.createNestedObject("ac"); ac["zero_cross"] = cfg.ac.zero_cross; ac["half_cycle"] = cfg.ac.half_cycle;
    JsonObject dc = d.createNestedObject("dc"); dc["i_limit"] = cfg.dc.i_limit; dc["pwm"] = cfg.dc.pwm;
    JsonObject sc = d.createNestedObject("sc"); sc["precharge"] = cfg.sc.precharge; sc["joule"] = cfg.sc.joule;
    JsonObject sensors = d.createNestedObject("sensors");
    sensors["zmpt"] = cfg.sensors.zmpt; sensors["acs712"] = cfg.sensors.acs712;
    sensors["ina219"] = cfg.sensors.ina219; sensors["ads1115"] = cfg.sensors.ads1115; sensors["web_beep"] = cfg.sensors.web_beep;
    JsonObject guards = d.createNestedObject("guards");
    guards["v_cutoff"] = cfg.guards.v_cutoff; guards["i_guard"] = cfg.guards.i_guard; guards["mcb_guard"] = cfg.guards.mcb_guard;
    JsonObject aut = d.createNestedObject("aut");
    aut["enabled"] = cfg.aut.enabled; aut["i_thresh_arms"] = cfg.aut.i_thresh_arms; aut["v_cutin_vrms"] = cfg.aut.v_cutin_vrms;
    aut["settle_ms"] = cfg.aut.settle_ms; aut["retrig_ms"] = cfg.aut.retrig_ms;
    JsonObject slots = d.createNestedObject("slots"); slots["active"] = cfg.active_slot;
    String j; serializeJson(d, j); req->send(200, "application/json", j);
  });

  server.on("/api/config/backend", HTTP_POST, [](AsyncWebServerRequest* req){ req->send(204); }, NULL,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t){
      DynamicJsonDocument d(2048);
      DeserializationError e = deserializeJson(d, data, len);
      if (e) { req->send(400); return; }
      cfg.backend = d["backend"].as<String>();
      cfg.ac.zero_cross = d["ac"]["zero_cross"].as<bool>();
      cfg.ac.half_cycle = d["ac"]["half_cycle"].as<bool>();
      cfg.dc.i_limit = d["dc"]["i_limit"].as<float>();
      cfg.dc.pwm = d["dc"]["pwm"].as<uint8_t>();
      cfg.sc.precharge = d["sc"]["precharge"].as<bool>();
      cfg.sc.joule = d["sc"]["joule"].as<float>();
      saveConfig();
    }
  );

  server.on("/api/config/sensors", HTTP_POST, [](AsyncWebServerRequest* req){ req->send(204); }, NULL,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t){
      DynamicJsonDocument d(1024);
      if (deserializeJson(d, data, len)) { req->send(400); return; }
      cfg.sensors.zmpt = d["sensors"]["zmpt"].as<bool>();
      cfg.sensors.acs712 = d["sensors"]["acs712"].as<bool>();
      cfg.sensors.ina219 = d["sensors"]["ina219"].as<bool>();
      cfg.sensors.ads1115 = d["sensors"]["ads1115"].as<bool>();
      cfg.sensors.web_beep = d["web_beep"].as<bool>();
      saveConfig();
    }
  );

  server.on("/api/config/guards", HTTP_POST, [](AsyncWebServerRequest* req){ req->send(204); }, NULL,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t){
      DynamicJsonDocument d(512);
      if (deserializeJson(d, data, len)) { req->send(400); return; }
      cfg.guards.v_cutoff = d["guards"]["v_cutoff"].as<float>();
      cfg.guards.i_guard = d["guards"]["i_guard"].as<float>();
      cfg.guards.mcb_guard = d["guards"]["mcb_guard"].as<bool>();
      saveConfig();
    }
  );

  server.on("/api/config/auto_trigger", HTTP_POST, [](AsyncWebServerRequest* req){ req->send(204); }, NULL,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t){
      DynamicJsonDocument d(512);
      if (deserializeJson(d, data, len)) { req->send(400); return; }
      cfg.aut.enabled = d["aut"]["ac"]["enabled"].as<bool>();
      cfg.aut.i_thresh_arms = d["aut"]["ac"]["i_thresh_arms"].as<float>();
      cfg.aut.v_cutin_vrms = d["aut"]["ac"]["v_cutin_vrms"].as<float>();
      cfg.aut.settle_ms = d["aut"]["ac"]["settle_ms"].as<uint16_t>();
      cfg.aut.retrig_ms = d["aut"]["ac"]["retrig_ms"].as<uint16_t>();
      saveConfig();
    }
  );

  // Cycle control (stub for W0/W1)
  server.on("/api/cycle/trigger", HTTP_POST, [](AsyncWebServerRequest* req){ req->send(202); });
  server.on("/api/cycle/abort", HTTP_POST, [](AsyncWebServerRequest* req){ req->send(202); });

  // Static files
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  // WebSocket telemetry (dummy for W0/W1)
  ws.onEvent([](AsyncWebSocket* s, AsyncWebSocketClient* c, AwsEventType t, void* p, uint8_t* d, size_t l){
    if (t == WS_EVT_CONNECT) {
      // send initial dummy telemetry
      DynamicJsonDocument j(256); j["type"] = "telemetry"; j["state"] = "IDLE"; j["vrms"] = 0; j["irms"] = 0;
      String out; serializeJson(j, out); c->text(out);
    }
  });
  server.addHandler(&ws);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("
[WSeries] Boot FW=%s
", FW_ID);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  } else {
    Serial.println("LittleFS mounted");
  }

  loadConfig();

  // SoftAP
  String ssid = String(AP_SSID_PREFIX) + macSuffix();
  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(ssid.c_str(), AP_PASS, AP_CHANNEL);
  Serial.printf("AP %s %s
", ssid.c_str(), ok?"OK":"FAIL");
  Serial.print("IP: "); Serial.println(WiFi.softAPIP());

  setupRoutes();
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  // In W0/W1, no FSM yet. Telemetry tick every ~1s (dummy)
  static uint32_t last = 0; uint32_t now = millis();
  if (now - last > 1000) {
    last = now;
    DynamicJsonDocument j(256); j["type"] = "telemetry"; j["state"] = "IDLE"; j["vrms"] = 0; j["irms"] = 0;
    String out; serializeJson(j, out);
    ws.textAll(out);
  }
}
