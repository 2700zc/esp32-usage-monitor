#pragma once
#include <Arduino.h>
#include "usage_types.h"

struct OpenCodeUsage {
  bool valid;
  UsageSegment rolling;   // 滚动用量
  UsageSegment weekly;    // 每周用量
  UsageSegment monthly;   // 每月用量
  uint32_t fetchedAt;     // 最近一次成功刷新的 millis()
  int lastError;          // 见 ocErr* 常量
  int lastHttp;           // 最近一次 HTTP 状态码（网络失败时为 0）
  char lastDetail[64];    // 最近一次失败的附加信息
};
enum { ocErrOk = 0, ocErrNoToken, ocErrNetwork, ocErrHttp, ocErrAuth, ocErrParse };

bool ocFetch(OpenCodeUsage& out, const char* workspaceId, const char* cookie, const char* serverId);
