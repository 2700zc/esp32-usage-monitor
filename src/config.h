#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

struct AppConfig {
  char ds_token[256];        // DeepSeek 平台 Bearer token（不含 "Bearer " 前缀）
  char oc_api_key[96];       // OpenCode Go 的 API key（oc_sk_ 开头）——首选凭据
  char oc_workspace_id[48];  // OpenCode org id，形如 wrk_XXXXXXXXXXXX（仅 cookie 方式用）
  char oc_cookie[2048];      // __Host-console_session 的值，或整串 cookie / 整条 curl
                             // 缓冲区按实测留足：浏览器整串 cookie 实测 763 字符，
                             // 而 session 通常排在末尾（偏移 644），缓冲区贴着长度
                             // 就会把 session 截断成"看似配好、请求必 401"的状态
  char cc_cookie[160];       // CommandCode 会话 token（只填 cookie 的值，不含 cookie 名）
  char agent_url[96];        // ZCode 状态桥接地址，形如 http://192.168.1.10:8787/agent
                             // （PC 上跑 tools/agent-bridge/bridge.py，启动时会打印
                             // 这一行，直接粘过来即可）
  bool valid;                // 至少配置了一个服务商
};

// 两条路任意一条走通就生成 OpenCode 页：API key 直接能用；会话 cookie 还得知道
// workspace_id（它进 x-org-id 与 Referer）。cookie 里能不能挑出 __Host-console_session
// 由 ocFetch 判定——这里只做非空检查，填了但填错时应该看到原因，而不是整页消失。
inline bool cfgOcReady(const AppConfig& c) {
  if (c.oc_api_key[0]) return true;
  return c.oc_cookie[0] && c.oc_workspace_id[0];
}
inline bool cfgCcReady(const AppConfig& c) { return c.cc_cookie[0] != 0; }
inline bool cfgDsReady(const AppConfig& c) { return c.ds_token[0] != 0; }
// 状态页总是存在：板子会在局域网里自己广播找桥接，所以不填 agent_url 也能用；
// 填了则先用它，取不到再退回发现（换电脑/换 IP 时不用重刷文件系统）。
inline bool cfgAgentReady(const AppConfig&) { return true; }

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

// curl 命令里带 cmd 风格的续行转义（^ 后跟换行），把整条粘贴进 config.json 时
// cookie 名与它的值可能被拆到两行，扫描就接不上了。规则与 TokenHub 的一致：
//   ^ + 空白 + 换行  → 整体删掉（这就是续行）
//   ^ + 其它字符     → 只删掉 '^'（cmd 用 ^" 转义引号）
// cookie 值本身不会含 '^'——浏览器会把它编码成 %5E。
static inline void stripCarets(char* s) {
  char* w = s;
  for (const char* r = s; *r; ) {
    if (*r != '^') { *w++ = *r++; continue; }
    const char* nx = r + 1;
    while (*nx == ' ' || *nx == '\t') nx++;
    if (*nx == '\r' || *nx == '\n') {
      r = (*nx == '\r' && nx[1] == '\n') ? nx + 2 : nx + 1;
    } else {
      r++;
    }
  }
  *w = 0;
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
  copyTrimmed(cfg.oc_api_key,      sizeof(cfg.oc_api_key),      doc["oc_api_key"]      | "");
  copyTrimmed(cfg.oc_workspace_id, sizeof(cfg.oc_workspace_id), doc["oc_workspace_id"] | "");
  copyTrimmed(cfg.oc_cookie,       sizeof(cfg.oc_cookie),       doc["oc_cookie"]       | "");
  stripCarets(cfg.oc_cookie);
  copyTrimmed(cfg.cc_cookie,       sizeof(cfg.cc_cookie),       doc["cc_cookie"]       | "");
  copyTrimmed(cfg.agent_url,       sizeof(cfg.agent_url),       doc["agent_url"]       | "");
  cfg.valid = cfgOcReady(cfg) || cfgCcReady(cfg) || cfgDsReady(cfg) || cfgAgentReady(cfg);
  return true;
}
