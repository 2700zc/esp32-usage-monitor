#include "agent_client.h"
#include "http_client.h"
#include <ArduinoJson.h>
#include <stdlib.h>
#include <string.h>

const char AGENT_PROBE[] = "ZCODE-AGENT-V1?";

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
