#pragma once
#include <Arduino.h>
#include <stddef.h>

// 单条请求头。name/value 必须活到 httpGet*() 返回——内部只存指针，不复制。
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

// 明文 HTTP GET，给局域网内的桥接进程用（Agent 状态轮询）。三个服务商都走
// HTTPS，只有这条不需要 TLS。超时默认短得多：它每秒都可能发一次，一旦桥接
// 不在（PC 关机、IP 变了）不能让主循环长时间卡住。
int httpGetPlain(const char* tag, const char* host, uint16_t port, const char* path,
                 const HttpHeader* headers, size_t nHeaders,
                 String& body, uint32_t timeoutMs = 1200);
