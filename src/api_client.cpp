#include "api_client.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <stdlib.h>

extern "C" void handleHttpStatus();

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
  uint32_t lastPoll = 0;
  String response;
  while (millis() - start < 10000) {
    uint32_t now = millis();
    if (now - lastPoll >= 100) {
      handleHttpStatus();
      lastPoll = now;
    }
    if (client.available()) {
      char c = client.read();
      response += c;
      if (response.length() > 8192) break;
    } else if (!client.connected()) {
      break;
    }
    delay(1);
  }
  client.stop();

  int bodyIdx = response.indexOf("\r\n\r\n");
  if (bodyIdx < 0) {
    return false;
  }
  const char* body = response.c_str() + bodyIdx + 4;

  parseUsage(body, out);
  return out.valid;
}
