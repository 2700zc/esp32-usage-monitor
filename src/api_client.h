#pragma once
#include <stdint.h>
#include <stddef.h>

struct UsageData {
  bool valid;
  int rollingPercent;
  uint32_t rollingResetSec;
  int weeklyPercent;
  uint32_t weeklyResetSec;
  int monthlyPercent;
  uint32_t monthlyResetSec;
};

bool apiFetchUsage(UsageData& out, const char* serverId, const char* cookie, const char* workspaceId);

bool apiBaiduStt(const uint8_t* audioData, size_t audioLen,
                 char* textOut, size_t textMax,
                 const char* apiKey, const char* secretKey);
