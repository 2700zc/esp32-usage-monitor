#pragma once
#include <Arduino.h>
#include "usage_types.h"

struct CommandCodeUsage {
  bool valid;
  UsageSegment fiveHour;  // 滚动用量（5 小时窗口）
  UsageSegment weekly;    // 每周用量
  UsageSegment monthly;   // 每月用量
  uint32_t fetchedAt;     // 最近一次成功刷新的 millis()
  int lastError;          // 见 ccErr* 常量
  int lastHttp;           // 最近一次 HTTP 状态码（网络失败时为 0）
  char lastDetail[64];    // 最近一次失败的附加信息
};
enum { ccErrOk = 0, ccErrNoToken, ccErrNetwork, ccErrHttp, ccErrAuth, ccErrParse };

bool ccFetch(CommandCodeUsage& out, const char* token);
