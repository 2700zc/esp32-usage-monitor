#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

struct AppConfig {
  char server_id[256];
  char cookie[2048];
  char workspace_id[128];
  char baidu_app_id[32];
  char baidu_api_key[64];
  char baidu_secret_key[64];
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
  strlcpy(cfg.baidu_app_id,  doc["baidu_app_id"]  | "", sizeof(cfg.baidu_app_id));
  strlcpy(cfg.baidu_api_key, doc["baidu_api_key"] | "", sizeof(cfg.baidu_api_key));
  strlcpy(cfg.baidu_secret_key, doc["baidu_secret_key"] | "", sizeof(cfg.baidu_secret_key));
  cfg.valid = cfg.server_id[0] != 0;
  return true;
}
