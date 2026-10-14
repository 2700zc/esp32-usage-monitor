#pragma once
#include <Arduino.h>
#include "usage_types.h"

struct OpenCodeUsage {
  bool valid;
  UsageSegment rolling;   // 滚动用量（上游叫 fiveHour，5 小时窗口）
  UsageSegment weekly;    // 每周用量（week）
  UsageSegment monthly;   // 每月用量（month）
  uint32_t fetchedAt;     // 最近一次成功刷新的 millis()
  int lastError;          // 见 ocErr* 常量
  int lastHttp;           // 最近一次 HTTP 状态码（网络失败时为 0）
  char lastDetail[64];    // 最近一次失败的附加信息
};
enum { ocErrOk = 0, ocErrNoToken, ocErrNetwork, ocErrHttp, ocErrAuth, ocErrParse, ocErrNoSession };

// 凭据有两种，apiKey 非空就用它（推荐，长期有效）：
//   apiKey   Go 套餐的 API key（oc_sk_ 开头），以 Authorization: Bearer 发送
//   cookie   浏览器会话：整串 cookie / 整条 curl / 裸 __Host-console_session 值，
//            由 ocFetch 自己挑出 session（见 extractSession）
bool ocFetch(OpenCodeUsage& out, const char* workspaceId, const char* cookie, const char* apiKey);
