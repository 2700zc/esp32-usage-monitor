#include "http_client.h"
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

// 正文上限 64 KB。三个服务商的响应通常都在 20 KB 以内。
//
// 注意 DeepSeek 那个接口不是固定大小：by_api_key/amount 返回的是「24 小时 ×
// 模型数」的完整 bucket 网格，跟用没用满无关。实测 2026-09-22 有 6 个模型、
// 144 个 bucket、共 18301 字节。也就是说体积随 DeepSeek 模型列表长度线性增长，
// 模型数翻倍就是 36 KB。
//
// 另外下面 body = response.substring(...) 会再复制一份，峰值占用是 2 倍，
// 所以真正可承载的正文只有约 32 KB 量级。真要顶到上限，表现是正文被截断 ->
// JSON 解析失败（界面显示"数据解析失败"），而不是报大小超限。到那时把这里改成
// 原地解析（传指针+长度，不做 substring 拷贝）比单纯调大上限更稳。
static const size_t HTTP_MAX_BODY = 65536;

// 建连之外的全部逻辑。TLS 与明文只差建连那几行，读写与解析共用这一份——
// 下面这段按 Content-Length 精确读取的循环有几个反复踩过的坑，复制一份出来
// 迟早会漂移。
static int httpGetOn(Client& client, const char* tag, const char* host, const char* path,
                     const HttpHeader* headers, size_t nHeaders,
                     String& body, uint32_t timeoutMs) {
  client.print("GET ");
  client.print(path);
  client.println(" HTTP/1.1");
  client.print("Host: ");
  client.println(host);
  for (size_t i = 0; i < nHeaders; i++) {
    client.print(headers[i].name);
    client.print(": ");
    client.println(headers[i].value);
  }
  client.println("Connection: close");
  client.println();

  // 按 Content-Length 精确读取：TLS 下 connected() 在 FIN 到达时可能
  // 提前返回 false，导致最后一段数据丢失（表现为 json:incompleteInput）
  uint32_t start = millis();
  String response;
  int headerEnd = -1;
  int contentLength = -1;
  bool overflow = false;
  while (millis() - start < timeoutMs) {
    while (client.available()) {
      response += (char)client.read();
      if (response.length() > HTTP_MAX_BODY) { overflow = true; break; }
    }
    if (overflow) break;
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
  while (!overflow && client.available() && millis() - start < timeoutMs) {
    response += (char)client.read();
  }
  client.stop();

  if (overflow) Serial.printf("%s: body over %u bytes, aborted\n", tag, (unsigned)HTTP_MAX_BODY);
  if (headerEnd < 0) return 0;

  int status = atoi(response.c_str() + response.indexOf(' ') + 1);
  if (contentLength > 0) {
    body = response.substring(headerEnd + 4, headerEnd + 4 + contentLength);
  } else {
    body = response.substring(headerEnd + 4);
  }

  // 日志只打 path 的问号前部分：OpenCode 的 args 查询串有 300+ 字符，全打太吵
  char pathLog[48];
  int q = path ? (int)strcspn(path, "?") : 0;
  if (q > (int)sizeof(pathLog) - 1) q = (int)sizeof(pathLog) - 1;
  if (path) { memcpy(pathLog, path, q); pathLog[q] = 0; } else pathLog[0] = 0;
  Serial.printf("%s: %s -> HTTP %d, %d bytes (cl=%d)\n", tag, pathLog, status,
                body.length(), contentLength);
  return status;
}

int httpGetSecure(const char* tag, const char* host, const char* path,
                  const HttpHeader* headers, size_t nHeaders,
                  String& body, uint32_t timeoutMs) {
  body = "";

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(timeoutMs);

  if (!client.connect(host, 443, timeoutMs)) {
    Serial.printf("%s: connect %s failed\n", tag, host);
    return 0;
  }
  return httpGetOn(client, tag, host, path, headers, nHeaders, body, timeoutMs);
}

int httpGetPlain(const char* tag, const char* host, uint16_t port, const char* path,
                 const HttpHeader* headers, size_t nHeaders,
                 String& body, uint32_t timeoutMs) {
  body = "";

  WiFiClient client;
  client.setTimeout(timeoutMs);

  if (!client.connect(host, port, timeoutMs)) {
    Serial.printf("%s: connect %s:%u failed\n", tag, host, (unsigned)port);
    return 0;
  }
  return httpGetOn(client, tag, host, path, headers, nHeaders, body, timeoutMs);
}
