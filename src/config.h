#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

struct AppConfig {
  char server_id[256];
  char cookie[2048];
  char workspace_id[128];
  char pc_host[64];
  uint16_t pc_port;
  bool valid;
};

inline bool loadConfig(AppConfig& cfg) {
  cfg.valid = false;
  if (!LittleFS.begin(false)) {
    if (!LittleFS.format()) return false;
    if (!LittleFS.begin(false)) return false;
  }
  File f = LittleFS.open("/config.json", "r");
  if (!f) return false;
  JsonDocument doc;
  if (deserializeJson(doc, f) != DeserializationError::Ok) {
    f.close(); return false;
  }
  f.close();
  strlcpy(cfg.server_id,     doc["server_id"]     | "", sizeof(cfg.server_id));
  strlcpy(cfg.cookie,        doc["cookie"]        | "", sizeof(cfg.cookie));
  strlcpy(cfg.workspace_id,  doc["workspace_id"]  | "", sizeof(cfg.workspace_id));
  strlcpy(cfg.pc_host,      doc["pc_host"]      | "", sizeof(cfg.pc_host));
  cfg.pc_port = (uint16_t)(doc["pc_port"] | 12345);
  cfg.valid = cfg.server_id[0] != 0;
  return true;
}
