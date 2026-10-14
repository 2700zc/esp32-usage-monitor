#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// 服务商响应里的数字字段：OpenCode 的 MicroCents（"1200000000"）与 CommandCode
// 的部分 credits 字段都是字符串，DeepSeek 是纯数字，两种都要吃。
// 统一用 double 承接——OpenCode 月度上限 6e9 超 uint32，而 6e9 在 double 下精确。
inline double jnum(JsonVariant v) {
  if (v.isNull()) return 0;
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    return s ? strtod(s, nullptr) : 0;
  }
  return v.as<double>();
}

// days-from-civil（Howard Hinnant）。不依赖 newlib 的 timegm——它在部分
// ESP32 工具链里没导出。
inline int64_t daysFromCivil(int y, int m, int d) {
  y -= (m <= 2);
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

// 解析 "2026-10-07T13:43:17.000Z"（UTC，毫秒部分忽略）
inline bool parseIsoUtc(const char* s, time_t& out) {
  int y, mo, d, h, mi, se;
  if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return false;
  out = (time_t)(daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se);
  return true;
}

// 距重置还有多少秒。NTP 未同步（time() 还停在 1970 附近）与已经过期都返回 0，
// 界面据此显示 "--"，绝不出现负数倒计时。time() 是 UTC epoch，与本机时区无关。
inline uint32_t resetSecUntil(time_t resetAt) {
  time_t now = time(nullptr);
  if (now < 100000 || resetAt <= 0) return 0;
  return resetAt > now ? (uint32_t)(resetAt - now) : 0;
}

inline uint32_t resetSecFromIso(const char* iso) {
  time_t ts;
  return (iso && iso[0] && parseIsoUtc(iso, ts)) ? resetSecUntil(ts) : 0;
}

// resetAt 是 epoch 毫秒（CommandCode 的 windowLimits）
inline uint32_t resetSecFromMs(double epochMs) {
  return epochMs > 0 ? resetSecUntil((time_t)(epochMs / 1000.0)) : 0;
}
