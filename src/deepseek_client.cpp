#include "deepseek_client.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <stdlib.h>
#include <time.h>

static const char* DS_HOST = "platform.deepseek.com";

// 鉴权实测结论：Authorization: Bearer 是唯一凭据，cookie 不需要；
// 但 User-Agent / Referer / x-client-* 头缺失会被 WAF 以 429 拦截，必须全带。
static void sendHeaders(WiFiClientSecure& client, const char* token, const char* path) {
  client.print("GET ");
  client.print(path);
  client.println(" HTTP/1.1");
  client.print("Host: ");
  client.println(DS_HOST);
  client.println("User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/152.0.0.0 Safari/537.36 Edg/152.0.0.0");
  client.println("Accept: */*");
  client.println("Referer: https://platform.deepseek.com/usage");
  client.print("Authorization: Bearer ");
  client.println(token);
  client.println("x-client-bundle-id: com.deepseek.chat");
  client.println("x-client-locale: zh_CN");
  client.println("x-client-platform: web");
  client.println("x-client-timezone-offset: 28800");
  client.println("x-client-version: 1.0.0");
  client.println("Connection: close");
  client.println();
}

// 东八区今天 0 点的 UTC 秒；NTP 未同步时返回 0
static uint32_t todayStartSec() {
  time_t ts = time(nullptr);
  if (ts < 100000) return 0;
  struct tm lt;
  localtime_r(&ts, &lt);
  return (uint32_t)(ts - (lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec));
}

// GET 请求，成功返回 HTTP 状态码并填充 body；TCP/TLS 失败返回 0
static int httpGet(const char* path, const char* token, String& body) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10000);

  if (!client.connect(DS_HOST, 443, 10000)) {
    Serial.printf("ds: connect %s failed\n", DS_HOST);
    return 0;
  }

  sendHeaders(client, token, path);

  // 按 Content-Length 精确读取：TLS 下 connected() 在 FIN 到达时可能
  // 提前返回 false，导致最后一段数据丢失（表现为 json:incompleteInput）
  uint32_t start = millis();
  String response;
  int headerEnd = -1;
  int contentLength = -1;
  while (millis() - start < 10000) {
    while (client.available()) {
      response += (char)client.read();
      if (response.length() > 65536) break;
    }
    if (headerEnd < 0) {
      headerEnd = response.indexOf("\r\n\r\n");
      if (headerEnd >= 0) {
        int clIdx = response.indexOf("Content-Length:");
        if (clIdx >= 0 && clIdx < headerEnd)
          contentLength = atoi(response.c_str() + clIdx + 15);
      }
    }
    int bodyLen = headerEnd >= 0 ? response.length() - (headerEnd + 4) : 0;
    if (headerEnd >= 0 && (contentLength >= 0 ? bodyLen >= contentLength : !client.connected()))
      break;
    if (headerEnd < 0 && !client.connected()) break;
    delay(1);
  }
  // 连接关闭后清空残余缓冲
  while (client.available() && millis() - start < 10000) {
    response += (char)client.read();
  }
  client.stop();

  if (headerEnd < 0) return 0;
  int status = atoi(response.c_str() + response.indexOf(' ') + 1);
  if (contentLength > 0) {
    body = response.substring(headerEnd + 4, headerEnd + 4 + contentLength);
  } else {
    body = response.substring(headerEnd + 4);
  }
  Serial.printf("ds: %s -> HTTP %d, %d bytes (cl=%d)\n", path, status, body.length(), contentLength);
  return status;
}

static bool parseAmount(const String& body, DeepSeekUsage& out) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err != DeserializationError::Ok) {
    snprintf(out.lastDetail, sizeof(out.lastDetail), "json:%s", err.c_str());
    Serial.printf("ds: amount json error: %s\n", err.c_str());
    return false;
  }
  JsonObject biz = doc["data"]["biz_data"];
  if (biz.isNull() || (int)(doc["data"]["biz_code"] | 1) != 0) {
    int code = (int)(doc["data"]["biz_code"] | 1);
    snprintf(out.lastDetail, sizeof(out.lastDetail), "biz_code=%d", code);
    Serial.printf("ds: amount biz_code=%d\n", code);
    return false;
  }

  uint64_t tok = 0;
  uint32_t req = 0;
  for (JsonObject series : biz["series"].as<JsonArray>()) {
    for (JsonObject b : series["buckets"].as<JsonArray>()) {
      JsonObject u = b["usage"];
      req += (uint32_t)(u["REQUEST"] | 0);
      tok += (uint64_t)(u["PROMPT_CACHE_HIT_TOKEN"] | 0);
      tok += (uint64_t)(u["PROMPT_CACHE_MISS_TOKEN"] | 0);
      tok += (uint64_t)(u["RESPONSE_TOKEN"] | 0);
    }
  }
  out.requestCount = req;
  out.tokenTotal = tok;
  return true;
}

static bool parseSummary(const String& body, DeepSeekUsage& out) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err != DeserializationError::Ok) {
    snprintf(out.lastDetail, sizeof(out.lastDetail), "json:%s", err.c_str());
    Serial.printf("ds: summary json error: %s\n", err.c_str());
    return false;
  }
  JsonObject biz = doc["data"]["biz_data"];
  if (biz.isNull()) {
    snprintf(out.lastDetail, sizeof(out.lastDetail), "no biz_data");
    Serial.println("ds: summary no biz_data");
    return false;
  }

  // balance 是字符串，可能是科学计数法（如 "0E-16"），用 strtod 解析；
  // 钱包数组第一个不一定是目标币种（实测 USD 在 [0]、CNY 在 [1]），按币种累加
  double cny = 0, usd = 0;
  for (JsonObject w : biz["normal_wallets"].as<JsonArray>()) {
    const char* cur = w["currency"] | "";
    double v = strtod(w["balance"] | "0", nullptr);
    if (strcmp(cur, "CNY") == 0) cny += v;
    else if (strcmp(cur, "USD") == 0) usd += v;
  }
  for (JsonObject w : biz["bonus_wallets"].as<JsonArray>()) {
    const char* cur = w["currency"] | "";
    double v = strtod(w["balance"] | "0", nullptr);
    if (strcmp(cur, "CNY") == 0) cny += v;
    else if (strcmp(cur, "USD") == 0) usd += v;
  }
  if (cny > 0) {
    out.balance = cny;
    strlcpy(out.currency, "CNY", sizeof(out.currency));
  } else {
    out.balance = usd;
    strlcpy(out.currency, "USD", sizeof(out.currency));
  }
  return true;
}

bool dsFetchToday(DeepSeekUsage& out, const char* token) {
  out.valid = false;
  out.lastError = dsErrOk;
  out.lastHttp = 0;
  if (!token || !token[0]) {
    out.lastError = dsErrNoToken;
    Serial.println("ds: no token in config");
    return false;
  }

  uint32_t start = todayStartSec();
  if (start == 0) {
    out.lastError = dsErrNoTime;
    Serial.println("ds: NTP not synced yet");
    return false;
  }

  DeepSeekUsage tmp = {};
  String body;

  // end 必须是完整一天（start + 86400），传当前时刻会返回 INVALID_PARAM
  char path[96];
  snprintf(path, sizeof(path),
           "/api/v0/usage/by_api_key/amount?start=%lu&end=%lu&tz=28800",
           (unsigned long)start, (unsigned long)(start + 86400));
  int st = httpGet(path, token, body);
  if (st != 200) {
    out.lastError = st == 0 ? dsErrNetwork : dsErrHttp;
    out.lastHttp = st;
    Serial.printf("ds: amount failed (http=%d)\n", st);
    return false;
  }
  if (!parseAmount(body, tmp)) {
    out.lastError = dsErrParse;
    strlcpy(out.lastDetail, tmp.lastDetail, sizeof(out.lastDetail));
    return false;
  }

  st = httpGet("/api/v0/users/get_user_summary", token, body);
  if (st != 200) {
    out.lastError = st == 0 ? dsErrNetwork : dsErrHttp;
    out.lastHttp = st;
    Serial.printf("ds: summary failed (http=%d)\n", st);
    return false;
  }
  if (!parseSummary(body, tmp)) {
    out.lastError = dsErrParse;
    strlcpy(out.lastDetail, tmp.lastDetail, sizeof(out.lastDetail));
    return false;
  }

  tmp.valid = true;
  tmp.fetchedAt = millis();
  out = tmp;
  return true;
}
