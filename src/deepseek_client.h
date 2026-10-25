#pragma once
#include <stdint.h>

struct DeepSeekUsage {
  bool valid;
  uint64_t tokenTotal;    // 今日总 token（缓存命中 + 未命中 + 输出）
  uint32_t requestCount;  // 今日请求数
  double balance;         // 账户余额（可用 + 赠送）
  char currency[8];       // USD / CNY
  uint32_t fetchedAt;     // 最近一次成功刷新的 millis()
  int lastError;          // 最近一次失败原因，见 dsErr* 常量
  int lastHttp;           // 最近一次 HTTP 状态码（网络失败时为 0）
  char lastDetail[64];    // 最近一次失败的附加信息（JSON 错误/响应开头）
};

// lastError 取值
enum {
  dsErrOk = 0,       // 成功
  dsErrNoToken,      // config 里没有 token
  dsErrNoTime,       // NTP 未同步，算不出今日边界
  dsErrNetwork,      // TCP/TLS 连接失败
  dsErrHttp,         // HTTP 状态码非 200（看 lastHttp）
  dsErrParse,        // JSON 解析失败
};

// 拉取 DeepSeek 平台今日用量和余额；失败时 out.valid = false，lastError 说明原因
bool dsFetchToday(DeepSeekUsage& out, const char* token);
