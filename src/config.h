#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

struct AppConfig {
  char ds_token[256];  // DeepSeek 平台 Bearer token（不含 "Bearer " 前缀）
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
  strlcpy(cfg.ds_token, doc["ds_token"] | "", sizeof(cfg.ds_token));
  cfg.valid = cfg.ds_token[0] != 0;
  return true;
}
