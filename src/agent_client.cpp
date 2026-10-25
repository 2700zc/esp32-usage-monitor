#include "agent_client.h"
#include "http_client.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <stdlib.h>
#include <string.h>

const char AGENT_PROBE[] = "ZCODE-AGENT-V1?";

// NVS 命名空间独立于 WiFi 那份（"buddy"），别互相覆盖
static const char* AGENT_NVS_NS = "agent";
static Preferences s_prefs;

bool agLoadLastHost(char* host, size_t hostSz, uint16_t& port) {
  s_prefs.begin(AGENT_NVS_NS, true);  // 只读
  String h = s_prefs.getString("host", "");
  unsigned long p = s_prefs.getUShort("port", 0);
  s_prefs.end();
  if (h.length() == 0 || p == 0 || p > 65535) return false;
  strlcpy(host, h.c_str(), hostSz);
  port = (uint16_t)p;
  return true;
}

void agSaveLastHost(const char* host, uint16_t port) {
  char cur[48];
  uint16_t curPort = 0;
  if (agLoadLastHost(cur, sizeof(cur), curPort) &&
      strcmp(cur, host) == 0 && curPort == port) {
    return;  // 没变就不写：NVS 是闪存，能省一次擦写就省一次
  }
  s_prefs.begin(AGENT_NVS_NS, false);
  s_prefs.putString("host", host);
  s_prefs.putUShort("port", port);
  s_prefs.end();
  Serial.printf("agent: remembered bridge %s:%u\n", host, (unsigned)port);
}

// 应答前缀必须连端口一起整串匹配："ZCODE-AGENT-V1 8787"。只比前缀的话，
// 状态推送的 JSON 万一以同样的字符串开头就会被当成应答。
static const char AGENT_REPLY_PREFIX[] = "ZCODE-AGENT-V1 ";

bool agParseReply(const char* buf, int len, uint16_t& httpPort) {
  const size_t pl = strlen(AGENT_REPLY_PREFIX);
  if (len <= (int)pl) return false;
  if (strncmp(buf, AGENT_REPLY_PREFIX, pl) != 0) return false;
  char* end = nullptr;
  long p = strtol(buf + pl, &end, 10);
  if (end == buf + pl || p <= 0 || p > 65535) return false;
  httpPort = (uint16_t)p;
  return true;
}

// agent_url -> host / port / path。只支持明文 http://：桥接跑在局域网里，
// 用 TLS 既没意义（自签证书还得 setInsecure）也要多花几百毫秒握手，与
// "每秒轮询"这个节奏冲突。写成 https:// 直接报错而不是悄悄降级，免得
// 用户以为自己配对了。
bool agParseUrl(const char* url, char* host, size_t hostSz,
                uint16_t& port, char* path, size_t pathSz) {
  const char* p = url;
  if (strncmp(p, "http://", 7) == 0) {
    p += 7;
  } else if (strncmp(p, "https://", 8) == 0) {
    return false;
  }

  const char* slash = strchr(p, '/');
  size_t hostLen = slash ? (size_t)(slash - p) : strlen(p);
  if (hostLen == 0 || hostLen >= hostSz) return false;

  // host[:port]
  port = 80;
  const char* colon = (const char*)memchr(p, ':', hostLen);
  size_t nameLen = hostLen;
  if (colon) {
    nameLen = (size_t)(colon - p);
    long v = strtol(colon + 1, nullptr, 10);
    if (v <= 0 || v > 65535) return false;
    port = (uint16_t)v;
  }
  if (nameLen == 0 || nameLen >= hostSz) return false;
  memcpy(host, p, nameLen);
  host[nameLen] = 0;

  const char* pth = slash ? slash : "/";
  if (strlen(pth) >= pathSz) return false;
  strcpy(path, pth);
  return true;
}

// 轮询与推送共用：两边送的是同一份 JSON（见 bridge.py 的 status()）
bool agApply(AgentStatus& out, const char* json, bool pushed) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err != DeserializationError::Ok) {
    snprintf(out.lastDetail, sizeof(out.lastDetail), "json:%s", err.c_str());
    out.lastError = agErrParse;
    Serial.printf("ag: json error: %s\n", err.c_str());
    return false;
  }
  const char* state = doc["state"] | "";
  if (!state[0]) {
    snprintf(out.lastDetail, sizeof(out.lastDetail), "no state field");
    out.lastError = agErrParse;
    Serial.println("ag: payload has no state");
    return false;
  }

  strlcpy(out.state, state, sizeof(out.state));
  strlcpy(out.tool, doc["tool"] | "", sizeof(out.tool));
  strlcpy(out.action, doc["detail"] | "", sizeof(out.action));
  strlcpy(out.project, doc["project"] | "", sizeof(out.project));
  out.ageSec         = (uint32_t)(doc["age"] | 0);
  out.activeSessions = (uint8_t)(doc["active"] | 0);
  out.stale          = (bool)(doc["stale"] | false);
  out.pushed         = pushed;
  out.lastError      = agErrOk;
  out.lastHttp       = 0;
  out.lastDetail[0]  = 0;
  out.valid          = true;
  out.fetchedAt      = millis();
  return true;
}

bool agFetchHost(AgentStatus& out, const char* host, uint16_t port, const char* path) {
  // 失败不清 valid：界面继续显示上一次的状态，只把页头换成琥珀色的过期提示。
  String body;
  int st = httpGetPlain("ag", host, port, path, nullptr, 0, body);
  if (st != 200) {
    out.lastError = st == 0 ? agErrNetwork : agErrHttp;
    out.lastHttp = st;
    return false;
  }
  return agApply(out, body.c_str(), false);
}

// ── 动作接口 ──
// 只发预定义名字，token 走查询串（桥接同时校验）。失败按 HTTP 码分类，屏幕上
// 要能区分"令牌不对"和"前台不是 ZCode"——两者的补救方式完全不同。
AgActCode agSendAct(const char* name, const char* host, uint16_t port,
                    const char* token, int& httpStatus) {
  httpStatus = 0;
  if (!name || !name[0])   return agActFailed;
  if (!token || !token[0]) return agActNoToken;
  if (!host || !host[0])   return agActNoHost;

  char path[128];
  snprintf(path, sizeof(path), "/act?name=%s&tok=%s", name, token);

  String body;
  // 聚焦是 PowerShell + UIA：预热后约 0.5-1.5 秒，冷启动实测 2.6-2.9 秒。
  // 超时给到 6 秒，避免设备先放弃、桥接后脚又把键发出去。
  int st = httpGetPlain("act", host, port, path, nullptr, 0, body, 6000);
  httpStatus = st;
  switch (st) {
    case 200: return agActOk;
    case 0:   return agActNetwork;
    case 403: return agActDenied;
    case 404: return agActOldBridge;
    case 409: return agActForeground;
    case 422: return agActNoEdit;
    default:  return agActFailed;
  }
}

// ── 清空最近一次语音粘贴 ──
// 桥接只在"同一个前台窗口 + 距粘贴 2 分钟内"才回删，所以这里不需要再判断
// 前台程序；把桥接返回的错误原样带到屏幕上，用户能直接看懂原因。
bool agSendClear(const char* host, uint16_t port, const char* token,
                 int& httpStatus, int& chars, char* err, size_t errSz) {
  httpStatus = 0;
  chars = 0;
  if (err && errSz) err[0] = 0;

  if (!token || !token[0]) {
    if (err && errSz) snprintf(err, errSz, "未配置 act_token");
    return false;
  }
  if (!host || !host[0]) {
    if (err && errSz) snprintf(err, errSz, "还没找到电脑桥接");
    return false;
  }

  char path[128];
  snprintf(path, sizeof(path), "/clear?tok=%s", token);

  String body;
  int st = httpGetPlain("clear", host, port, path, nullptr, 0, body, 6000);
  httpStatus = st;
  if (st <= 0) {
    if (err && errSz) snprintf(err, errSz, "连不上电脑桥接");
    return false;
  }

  JsonDocument doc;
  DeserializationError je = deserializeJson(doc, body);
  if (!je) {
    chars = doc["chars"] | 0;
    if (st == 200 && (doc["ok"] | false)) return true;
    const char* e = doc["error"] | "";
    if (err && errSz) {
      if (e[0]) snprintf(err, errSz, "%s", e);
      else      snprintf(err, errSz, "清空失败 (HTTP %d)", st);
    }
    return false;
  }

  if (err && errSz) snprintf(err, errSz, "电脑返回异常 (HTTP %d)", st);
  return false;
}

const char* agActName(AgActCode code) {
  switch (code) {
    case agActOk:         return "ok";
    case agActNoToken:    return "no token";
    case agActNoHost:     return "no bridge";
    case agActNetwork:    return "network";
    case agActDenied:     return "denied";
    case agActForeground: return "foreground";
    case agActNoEdit:     return "no focused input";
    case agActOldBridge:  return "old bridge";
    default:              return "failed";
  }
}
