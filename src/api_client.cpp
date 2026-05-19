#include "api_client.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <stdlib.h>

static int extractIntAfter(const char* str, const char* prefix, const char* key) {
  if (!str || !prefix || !key) return -1;
  const char* p = strstr(str, prefix);
  if (!p) return -1;
  p += strlen(prefix);
  const char* k = strstr(p, key);
  if (!k) return -1;
  k += strlen(key);
  const char* end = k;
  while (*end >= '0' && *end <= '9') end++;
  if (end == k) return -1;
  char saved = *end;
  *((char*)end) = 0;
  int val = atoi(k);
  *((char*)end) = saved;
  return val;
}

static void parseUsage(const char* body, UsageData& out) {
  out.valid = false;

  int rp = extractIntAfter(body, "rollingUsage", "usagePercent:");
  int rr = extractIntAfter(body, "rollingUsage", "resetInSec:");
  int wp = extractIntAfter(body, "weeklyUsage", "usagePercent:");
  int wr = extractIntAfter(body, "weeklyUsage", "resetInSec:");
  int mp = extractIntAfter(body, "monthlyUsage", "usagePercent:");
  int mr = extractIntAfter(body, "monthlyUsage", "resetInSec:");

  if (rp < 0 || rr < 0 || wp < 0 || wr < 0 || mp < 0 || mr < 0) return;

  out.rollingPercent  = rp;
  out.rollingResetSec = (uint32_t)rr;
  out.weeklyPercent   = wp;
  out.weeklyResetSec  = (uint32_t)wr;
  out.monthlyPercent  = mp;
  out.monthlyResetSec = (uint32_t)mr;
  out.valid = true;
}

bool apiFetchUsage(UsageData& out, const char* serverId, const char* cookie, const char* workspaceId) {
  out.valid = false;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10000);

  const char* host = "opencode.ai";

  if (!client.connect(host, 443, 10000)) return false;

  String args = "%7B%22t%22%3A%7B%22t%22%3A9%2C%22i%22%3A0%2C%22l%22%3A1%2C%22a%22%3A%5B%7B%22t%22%3A1%2C%22s%22%3A%22";
  args += workspaceId;
  args += "%22%7D%5D%2C%22o%22%3A0%7D%2C%22f%22%3A31%2C%22m%22%3A%5B%5D%7D";
  String path = "/_server?id=";
  path += serverId;
  path += "&args=";
  path += args;

  client.print("GET ");
  client.print(path);
  client.println(" HTTP/1.1");
  client.print("Host: ");
  client.println(host);
  client.println("User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/145.0.0.0 Safari/537.36");
  client.println("Accept: */*");
  client.println("Accept-Language: zh-CN,zh;q=0.9");
  client.print("x-server-id: ");
  client.println(serverId);
  client.println("x-server-instance: server-fn:4");
  if (cookie && strlen(cookie) > 0) {
    client.print("Cookie: ");
    client.println(cookie);
  }
  client.println("Connection: close");
  client.println();

  uint32_t start = millis();
  String response;
  while (millis() - start < 10000) {
    if (client.available()) {
      char c = client.read();
      response += c;
      if (response.length() > 8192) break;
    } else if (!client.connected()) {
      break;
    }
  }
  client.stop();

  Serial.printf("API response (%u bytes): %s\n", response.length(), response.c_str());

  int bodyIdx = response.indexOf("\r\n\r\n");
  if (bodyIdx < 0) {
    Serial.println("API: no header separator found");
    return false;
  }
  const char* body = response.c_str() + bodyIdx + 4;
  Serial.printf("API body: %s\n", body);

  parseUsage(body, out);
  return out.valid;
}

static String s_baiduToken;
static uint32_t s_tokenExpiry = 0;

static bool getBaiduToken(const char* apiKey, const char* secretKey) {
  if (s_baiduToken.length() > 0 && millis() < s_tokenExpiry) return true;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10000);

  if (!client.connect("aip.baidubce.com", 443)) return false;

  String body = "grant_type=client_credentials&client_id=";
  body += apiKey;
  body += "&client_secret=";
  body += secretKey;

  client.println("POST /oauth/2.0/token HTTP/1.1");
  client.println("Host: aip.baidubce.com");
  client.println("Content-Type: application/x-www-form-urlencoded");
  client.print("Content-Length: ");
  client.println(body.length());
  client.println("Connection: close");
  client.println();
  client.print(body);

  uint32_t start = millis();
  String resp;
  while (millis() - start < 10000) {
    if (client.available()) { char c = client.read(); resp += c; }
    else if (!client.connected()) break;
  }
  client.stop();

  int bodyIdx = resp.indexOf("\r\n\r\n");
  if (bodyIdx < 0) return false;
  const char* json = resp.c_str() + bodyIdx + 4;

  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return false;
  const char* token = doc["access_token"];
  if (!token) return false;
  s_baiduToken = token;
  s_tokenExpiry = millis() + 2500000;
  return true;
}

bool apiBaiduStt(const uint8_t* audioData, size_t audioLen,
                 char* textOut, size_t textMax,
                 const char* apiKey, const char* secretKey) {
  textOut[0] = 0;
  if (!audioData || audioLen == 0) return false;

  if (!getBaiduToken(apiKey, secretKey)) return false;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);

  if (!client.connect("vop.baidu.com", 443)) return false;

  String path = "/server_api?dev_pid=1537&token=";
  path += s_baiduToken;
  path += "&cuid=ESP32_USAGE_MONITOR";

  client.print("POST ");
  client.print(path);
  client.println(" HTTP/1.1");
  client.println("Host: vop.baidu.com");
  client.println("Content-Type: audio/pcm;rate=16000");
  client.print("Content-Length: ");
  client.println(audioLen);
  client.println("Connection: close");
  client.println();
  client.write(audioData, audioLen);

  uint32_t start = millis();
  String resp;
  while (millis() - start < 15000) {
    if (client.available()) { char c = client.read(); resp += c; }
    else if (!client.connected()) break;
  }
  client.stop();

  int bodyIdx = resp.indexOf("\r\n\r\n");
  if (bodyIdx < 0) return false;
  const char* json = resp.c_str() + bodyIdx + 4;

  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return false;
  int errNo = doc["err_no"] | -1;
  if (errNo != 0) {
    Serial.printf("Baidu STT error: %d\n", errNo);
    return false;
  }
  JsonArray result = doc["result"];
  if (result.isNull() || result.size() == 0) return false;
  const char* text = result[0];
  if (!text) return false;
  strlcpy(textOut, text, textMax);
  return true;
}
