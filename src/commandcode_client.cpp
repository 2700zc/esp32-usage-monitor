#include "commandcode_client.h"
#include "http_client.h"
#include <ArduinoJson.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char* CC_HOST = "api.commandcode.ai";
static const char* CC_TOKEN_COOKIE = "__Secure-commandcode_prod_.session_token";

// used / cap / monthlyCredits 实测是 JSON 数字，但字段名带 Credits 的几项
// 上游有返回字符串的先例，两种都吃。
static double jnum(JsonVariant v) {
  if (v.isNull()) return 0;
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    return s ? strtod(s, nullptr) : 0;
  }
  return v.as<double>();
}

// days-from-civil（Howard Hinnant）。不依赖 newlib 的 timegm——它在部分
// ESP32 工具链里没导出。
static int64_t daysFromCivil(int y, int m, int d) {
  y -= (m <= 2);
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

// 解析 "2026-10-07T13:43:17.000Z"（UTC，毫秒部分忽略）
static bool parseIsoUtc(const char* s, time_t& out) {
  int y, mo, d, h, mi, se;
  if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return false;
  out = (time_t)(daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se);
  return true;
}

// resetAt 是 epoch 毫秒。NTP 未同步时返回 0，界面显示 "--"。
static uint32_t resetSecFromMs(double resetAtMs) {
  time_t now = time(nullptr);
  if (now < 100000 || resetAtMs <= 0) return 0;
  double diff = resetAtMs / 1000.0 - (double)now;
  return diff > 0 ? (uint32_t)diff : 0;
}

static void makeSeg(UsageSegment& seg, double used, double cap, double resetAtMs) {
  if (cap <= 0) {
    strlcpy(seg.status, "unavailable", sizeof(seg.status));
    return;
  }
  seg.valid = true;
  seg.percent = (float)(used / cap * 100.0);
  seg.resetSec = resetSecFromMs(resetAtMs);
  strlcpy(seg.status, "ok", sizeof(seg.status));
}

// 实测（2026-09-22）：credits 与 windowLimits 是根对象下两个并列字段，
// windowLimits 不在 credits 里面。
static bool parseCredits(const String& body, CommandCodeUsage& out) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err != DeserializationError::Ok) {
    snprintf(out.lastDetail, sizeof(out.lastDetail), "json:%s", err.c_str());
    return false;
  }
  JsonObject wl = doc["windowLimits"];
  JsonObject cr = doc["credits"];
  if (wl.isNull() || cr.isNull()) {
    strlcpy(out.lastDetail, "缺 windowLimits/credits", sizeof(out.lastDetail));
    return false;
  }

  // limited=false 是按量付费、套餐无上限，不是用量为零
  if (!(wl["limited"] | false)) {
    strlcpy(out.fiveHour.status, "unlimited", sizeof(out.fiveHour.status));
    strlcpy(out.weekly.status,   "unlimited", sizeof(out.weekly.status));
    strlcpy(out.monthly.status,  "unlimited", sizeof(out.monthly.status));
    return true;
  }

  JsonObject fh = wl["fiveHour"];
  JsonObject wk = wl["weekly"];
  if (!fh.isNull()) makeSeg(out.fiveHour, jnum(fh["used"]), jnum(fh["cap"]), jnum(fh["resetAt"]));
  if (!wk.isNull()) makeSeg(out.weekly,   jnum(wk["used"]), jnum(wk["cap"]), jnum(wk["resetAt"]));

  double granted = jnum(cr["monthlyCreditsGranted"]);
  double left    = jnum(cr["monthlyCredits"]);
  // 月度没有 resetAt，倒计时由 subscriptions.currentPeriodEnd 补（见 ccFetch）
  if (granted > 0) makeSeg(out.monthly, granted - left, granted, 0);
  return true;
}

bool ccFetch(CommandCodeUsage& out, const char* token) {
  // 失败时不清 valid：保留上一次成功的数据供界面继续显示（见 dsFetchToday 的说明）
  out.lastError = ccErrOk;
  out.lastHttp = 0;
  out.lastDetail[0] = 0;

  if (!token || !token[0]) {
    out.lastError = ccErrNoToken;
    Serial.println("cc: 未配置 token");
    return false;
  }

  char cookie[192];
  snprintf(cookie, sizeof(cookie), "%s=%s", CC_TOKEN_COOKIE, token);

  HttpHeader h[] = {
    {"User-Agent",   "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"},
    {"Accept",       "*/*"},
    {"Content-Type", "application/json"},
    {"Origin",       "https://commandcode.ai"},
    {"Referer",      "https://commandcode.ai/"},
    {"Cookie",       cookie},
  };
  const size_t nH = sizeof(h) / sizeof(h[0]);

  String body;
  int st = httpGetSecure("cc", CC_HOST, "/internal/billing/credits", h, nH, body);
  if (st == 0) { out.lastError = ccErrNetwork; return false; }
  if (st == 401 || st == 403) { out.lastError = ccErrAuth; out.lastHttp = st; return false; }
  if (st != 200) { out.lastError = ccErrHttp; out.lastHttp = st; return false; }

  CommandCodeUsage tmp = {};
  if (!parseCredits(body, tmp)) {
    out.lastError = ccErrParse;
    strlcpy(out.lastDetail, tmp.lastDetail, sizeof(out.lastDetail));
    return false;
  }

  // 月度重置时间只在 subscriptions 里，且是 ISO-8601 字符串。这个接口是尽力而为：
  // 失败只让月度倒计时显示 --，不影响另外两个窗口。
  body = "";
  st = httpGetSecure("cc", CC_HOST, "/internal/billing/subscriptions", h, nH, body);
  if (st == 200) {
    JsonDocument doc;
    if (deserializeJson(doc, body) == DeserializationError::Ok) {
      const char* endIso = doc["data"]["currentPeriodEnd"] | "";
      time_t endTs;
      time_t now = time(nullptr);
      if (parseIsoUtc(endIso, endTs) && now >= 100000 && endTs > now)
        tmp.monthly.resetSec = (uint32_t)(endTs - now);
    }
  } else {
    Serial.printf("cc: subscriptions http=%d，月度倒计时保持 --\n", st);
  }

  tmp.valid = true;
  tmp.fetchedAt = millis();
  out = tmp;
  Serial.printf("cc: 5h=%.2f%% weekly=%.2f%% monthly=%.2f%%\n",
                tmp.fiveHour.percent, tmp.weekly.percent, tmp.monthly.percent);
  return true;
}
