#include "commandcode_client.h"
#include "http_client.h"
#include "json_util.h"
#include <ArduinoJson.h>
#include <string.h>
#include <time.h>

static const char* CC_HOST = "api.commandcode.ai";
static const char* CC_TOKEN_COOKIE = "__Secure-commandcode_prod_.session_token";

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
    if (deserializeJson(doc, body) == DeserializationError::Ok)
      tmp.monthly.resetSec = resetSecFromIso(doc["data"]["currentPeriodEnd"] | "");
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
