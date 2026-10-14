#include "opencode_client.h"
#include "http_client.h"
#include "json_util.h"
#include <ArduinoJson.h>
#include <string.h>

static const char* OC_HOST = "opencode.ai";
static const char* OC_SESSION_COOKIE = "__Host-console_session";

// session 值的形状：st_ 开头，后面全是十六进制与 '-'（实测 39 字符）。Python 版
// 认裸值时用的也是这条（st_[0-9a-f-]{20,}）。
// 校验的意义在于"粘贴被截断"这类情况：只取到半截 token 的话请求必是 401，
// 界面会说"cookie 已过期"，把人往错的方向引；先判形状就能报"缺少 session"。
static bool isSessionChar(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F') || c == '-';
}

static bool looksLikeSession(const char* s, size_t n) {
  if (n < 23 || strncmp(s, "st_", 3) != 0) return false;
  for (size_t i = 3; i < n; i++)
    if (!isSessionChar(s[i])) return false;
  return true;
}

// 从填进来的凭据里取出 __Host-console_session 的值，三种写法都认（与 TokenHub
// 的 extract_console_session 一致）：
//   1. 裸值           st_6d1759dd-3b3f-4c79-b109-40163b463cf3
//   2. 整串 cookie    oc_locale=zh; __Host-console_session=st_…; __stripe_mid=…
//   3. 整条 curl      （cookie 名可能跨行，靠名字定位即可）
// 旧版的 auth=Fe26.2**（_server 接口那套）不在识别范围内：那个接口 2026-10 起
// 只回 302 跳登录页，凭据换成 console 会话才有用量。
static bool extractSession(const char* raw, char* out, size_t sz) {
  if (!raw || !raw[0] || sz < 48) return false;

  const char* p = strstr(raw, OC_SESSION_COOKIE);
  if (p) {
    p += strlen(OC_SESSION_COOKIE);
    // 值前面可能隔着 =、"、' 与换行（整条 curl 被续行拆开时下一行以引号开头）
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' ||
           *p == '=' || *p == '"' || *p == '\'' || *p == '\\') p++;
    size_t n = 0;
    while (p[n] && n < sz - 1 && p[n] != ';' && p[n] != '"' && p[n] != '\'' &&
           p[n] != ' ' && p[n] != '\t' && p[n] != '\r' && p[n] != '\n') {
      n++;
    }
    // 顶到缓冲区上限说明值被截断了；形状不对则多半是粘贴被拆断，都交给下面的
    // 裸值扫描再试一次，最终由调用方报"缺少 session"
    if (n > 0 && !(n == sz - 1 && p[n]) && looksLikeSession(p, n)) {
      memcpy(out, p, n);
      out[n] = 0;
      return true;
    }
  }

  // 裸值：st_ 后面跟一串十六进制与 '-'（与 Python 的 st_[0-9a-f-]{20,} 同规则）。
  // 太短的多半是别的字符串里巧合出现的 "st_"
  for (const char* q = raw; (q = strstr(q, "st_")) != nullptr; q += 3) {
    size_t n = 0;
    while (n < 64 && isSessionChar(q[3 + n])) n++;
    if (n >= 20 && n + 4 <= sz) {
      memcpy(out, q, n + 3);
      out[n + 3] = 0;
      return true;
    }
  }
  return false;
}

// 一个用量窗口。上游实测响应（2026-10，883 字节）：
//   {"access":{"meters":{
//      "fiveHour":{"startsAt":"…","resetsAt":"2026-10-09T05:37:30.000Z",
//                  "limitMicroCents":"1200000000","usedMicroCents":"6510335"},
//      "week":{…},"month":{…}}}}
// 缺字段或上限为 0 按 unavailable 处理（界面显示 -- 与「额度不可用」），与 TokenHub
// 的 parse_go_status 一致；month 的 startsAt 实测就是缺的，别依赖它。
static void parseMeter(JsonObject m, UsageSegment& seg) {
  if (m.isNull()) {
    strlcpy(seg.status, "unavailable", sizeof(seg.status));
    return;
  }
  double cap  = jnum(m["limitMicroCents"]);
  double used = jnum(m["usedMicroCents"]);
  if (cap <= 0) {
    strlcpy(seg.status, "unavailable", sizeof(seg.status));
    return;
  }
  seg.valid    = true;
  seg.percent  = (float)(used / cap * 100.0);
  seg.resetSec = resetSecFromIso(m["resetsAt"] | "");
  strlcpy(seg.status, "ok", sizeof(seg.status));
}

bool ocFetch(OpenCodeUsage& out, const char* workspaceId, const char* cookie, const char* apiKey) {
  // 失败时不清 valid：保留上一次成功的数据供界面继续显示（见 dsFetchToday 的说明）
  out.lastError = ocErrOk;
  out.lastHttp = 0;
  out.lastDetail[0] = 0;

  // API key 优先：它是长期凭据，而 console 会话 cookie 会过期。实测（2026-10-09）
  // Bearer 方式连 x-org-id 与 Referer 都不需要，所以这条路径不依赖 workspace_id。
  const bool useKey = apiKey && apiKey[0];

  char auth[96];
  char session[128];
  char referer[128];
  char cookieHdr[160];
  HttpHeader hKey[3];
  HttpHeader hCookie[5];

  if (useKey) {
    snprintf(auth, sizeof(auth), "Bearer %s", apiKey);
    hKey[0] = {"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"};
    hKey[1] = {"Accept", "*/*"};
    hKey[2] = {"Authorization", auth};
  } else {
    if (!workspaceId || !workspaceId[0] || !cookie || !cookie[0]) {
      out.lastError = ocErrNoToken;
      Serial.println("oc: 既没配 api key，workspace_id / cookie 也不全");
      return false;
    }
    if (!extractSession(cookie, session, sizeof(session))) {
      out.lastError = ocErrNoSession;
      // 卡片里这一行只有约 160px，wqy12 下 22 个 ASCII 加 1 个汉字刚好放得下
      strlcpy(out.lastDetail, "需 __Host-console_session", sizeof(out.lastDetail));
      Serial.println("oc: cookie 里没找到 __Host-console_session（旧版 auth= 已失效）");
      return false;
    }
    snprintf(referer, sizeof(referer), "https://opencode.ai/console/%s/models", workspaceId);
    snprintf(cookieHdr, sizeof(cookieHdr), "%s=%s", OC_SESSION_COOKIE, session);
    hCookie[0] = {"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"};
    hCookie[1] = {"Accept", "*/*"};
    hCookie[2] = {"x-org-id", workspaceId};
    hCookie[3] = {"Referer", referer};
    hCookie[4] = {"Cookie", cookieHdr};
  }

  const HttpHeader* h = useKey ? hKey : hCookie;
  const size_t nH = useKey ? 3 : 5;

  String body;
  int st = httpGetSecure("oc", OC_HOST, "/console/api/go/status", h, nH, body);
  if (st == 0) { out.lastError = ocErrNetwork; return false; }
  // 会话过期时是 401；未登录访问则可能给一个跳登录页的 302
  if (st == 401 || st == 403 || st == 302) {
    out.lastError = ocErrAuth;
    out.lastHttp = st;
    strlcpy(out.lastDetail, useKey ? "换新 key 后重刷文件系统" : "重新复制后重刷文件系统",
            sizeof(out.lastDetail));
    return false;
  }
  if (st != 200) { out.lastError = ocErrHttp; out.lastHttp = st; return false; }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err != DeserializationError::Ok) {
    out.lastError = ocErrParse;
    snprintf(out.lastDetail, sizeof(out.lastDetail), "json:%s", err.c_str());
    Serial.printf("oc: json error: %s\n", err.c_str());
    return false;
  }

  JsonObject meters = doc["access"]["meters"];
  if (meters.isNull()) {
    out.lastError = ocErrParse;
    strlcpy(out.lastDetail, "缺 access.meters", sizeof(out.lastDetail));
    Serial.println("oc: 响应里没有 access.meters（套餐不是 Go？）");
    return false;
  }

  OpenCodeUsage tmp = {};
  parseMeter(meters["fiveHour"], tmp.rolling);
  parseMeter(meters["week"],     tmp.weekly);
  parseMeter(meters["month"],    tmp.monthly);

  tmp.valid = true;
  tmp.fetchedAt = millis();
  out = tmp;
  Serial.printf("oc[%s]: rolling=%.2f%% weekly=%.2f%% monthly=%.2f%%\n",
                useKey ? "key" : "cookie",
                tmp.rolling.percent, tmp.weekly.percent, tmp.monthly.percent);
  return true;
}
