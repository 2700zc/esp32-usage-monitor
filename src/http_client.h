#pragma once
#include <Arduino.h>
#include <stddef.h>

// 单条请求头。name/value 必须活到 httpGetSecure() 返回——内部只存指针，不复制。
struct HttpHeader {
  const char* name;
  const char* value;
};

// HTTPS GET。成功返回 HTTP 状态码并填充 body；TCP/TLS 连接失败返回 0。
// Host 与 Connection: close 由本函数补，headers 数组的顺序就是发送顺序。
// tag 只作串口日志前缀（"ds" / "oc" / "cc"）。
int httpGetSecure(const char* tag, const char* host, const char* path,
                  const HttpHeader* headers, size_t nHeaders,
                  String& body, uint32_t timeoutMs = 10000);
