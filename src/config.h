#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

struct AppConfig {
  char ds_token[256];        // DeepSeek 平台 Bearer token（不含 "Bearer " 前缀）
  char oc_workspace_id[48];  // OpenCode workspace，形如 wrk_XXXXXXXXXXXX
  char oc_server_id[80];     // server-fn id（64 位十六进制）
  char oc_cookie[768];       // OpenCode 浏览器 Cookie 整串（含 auth=Fe26.2**…）
  char cc_cookie[160];       // CommandCode 会话 token（只填 cookie 的值，不含 cookie 名）
  bool valid;                // 至少配置了一个服务商
};

// 三项都齐才能请求 OpenCode：cookie 鉴权，workspace_id 进 Referer 与 args，
// server_id 同时作查询参数和 X-Server-Id 头。任一项为空就不生成该页。
inline bool cfgOcReady(const AppConfig& c) {
  return c.oc_cookie[0] && c.oc_server_id[0] && c.oc_workspace_id[0];
}
inline bool cfgCcReady(const AppConfig& c) { return c.cc_cookie[0] != 0; }
inline bool cfgDsReady(const AppConfig& c) { return c.ds_token[0] != 0; }

// 复制并去掉首尾空白。手写 JSON 时很容易在值前后留下空格或换行，而" "是非空
// 字符串，会被当成"已配置"，于是生成一个必然失败的页面。
static inline void copyTrimmed(char* dst, size_t sz, const char* src) {
  if (!src) { dst[0] = 0; return; }
  while (*src == ' ' || *src == '\t' || *src == '\r' || *src == '\n') src++;
  size_t n = strlen(src);
  while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == '\t' ||
                   src[n - 1] == '\r' || src[n - 1] == '\n')) n--;
  if (n >= sz) n = sz - 1;
  memcpy(dst, src, n);
  dst[n] = 0;
}

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
  copyTrimmed(cfg.ds_token,        sizeof(cfg.ds_token),        doc["ds_token"]        | "");
  copyTrimmed(cfg.oc_workspace_id, sizeof(cfg.oc_workspace_id), doc["oc_workspace_id"] | "");
  copyTrimmed(cfg.oc_server_id,    sizeof(cfg.oc_server_id),    doc["oc_server_id"]    | "");
  copyTrimmed(cfg.oc_cookie,       sizeof(cfg.oc_cookie),       doc["oc_cookie"]       | "");
  copyTrimmed(cfg.cc_cookie,       sizeof(cfg.cc_cookie),       doc["cc_cookie"]       | "");
  cfg.valid = cfgOcReady(cfg) || cfgCcReady(cfg) || cfgDsReady(cfg);
  return true;
}
