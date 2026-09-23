#include "opencode_client.h"
#include "http_client.h"
#include <stdlib.h>
#include <string.h>

static const char* OC_HOST = "opencode.ai";

// ── URL 编码 ──
// 与 Python urllib.parse.quote 的默认 safe='/' 一致：unreserved 字符与 '/' 原样保留，
// 其余全部转 %XX。args 是 JSON，花括号引号冒号都会被编码。
static bool encAppend(char* dst, size_t dstSz, size_t& pos, const char* src) {
  // 名字不能叫 HEX——Arduino 的 Print.h 里 #define HEX 16
  static const char* hexDigits = "0123456789ABCDEF";
  for (const unsigned char* p = (const unsigned char*)src; *p; p++) {
    unsigned char c = *p;
    bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                c == '~' || c == '/';
    if (keep) {
      if (pos + 2 > dstSz) return false;
      dst[pos++] = (char)c;
    } else {
      if (pos + 4 > dstSz) return false;
      dst[pos++] = '%';
      dst[pos++] = hexDigits[c >> 4];
      dst[pos++] = hexDigits[c & 0x0F];
    }
  }
  dst[pos] = 0;
  return true;
}

// 返回与 openIdx 处 '{' 配对的 '}' 下标；字符串字面量里的花括号会被跳过
static int matchBrace(const String& s, int openIdx) {
  int depth = 0;
  int n = (int)s.length();
  for (int i = openIdx; i < n; i++) {
    char c = s[i];
    if (c == '"') {
      i++;
      while (i < n && s[i] != '"') {
        if (s[i] == '\\') i++;
        i++;
      }
      continue;
    }
    if (c == '{') {
      depth++;
    } else if (c == '}') {
      if (--depth == 0) return i;
    }
  }
  return -1;
}

// 读一个用量窗口。响应不是 JSON，是 SolidStart 的服务端函数文本：
//   rollingUsage:$R[2]={status:"ok",resetInSec:18000,usagePercent:0,usage:0,limit:1200000000}
// 所以先定位 key，再取其后那个对象的花括号范围，然后在范围内找三个字段——
// 不依赖字段顺序，也不依赖 $R[n] 的下标。
static bool scanSegment(const String& body, const char* key, UsageSegment& seg) {
  int k = body.indexOf(key);
  if (k < 0) return false;
  int ob = body.indexOf('{', k);
  if (ob < 0) return false;
  int cb = matchBrace(body, ob);
  if (cb < 0) return false;

  int sp = body.indexOf("status:\"", ob);
  if (sp >= 0 && sp < cb) {
    int v0 = sp + 8;
    int v1 = body.indexOf('"', v0);
    if (v1 > v0 && v1 <= cb) {
      int n = v1 - v0;
      if (n > (int)sizeof(seg.status) - 1) n = (int)sizeof(seg.status) - 1;
      memcpy(seg.status, body.c_str() + v0, n);
      seg.status[n] = 0;
    }
  }

  int rp = body.indexOf("resetInSec:", ob);
  if (rp >= 0 && rp < cb)
    seg.resetSec = (uint32_t)strtoul(body.c_str() + rp + 11, nullptr, 10);

  int pp = body.indexOf("usagePercent:", ob);
  if (pp >= 0 && pp < cb) {
    seg.percent = (float)strtod(body.c_str() + pp + 13, nullptr);
    seg.valid = true;
  }
  return seg.valid;
}

bool ocFetch(OpenCodeUsage& out, const char* workspaceId, const char* cookie, const char* serverId) {
  // 失败时不清 valid：保留上一次成功的数据供界面继续显示（见 dsFetchToday 的说明）
  out.lastError = ocErrOk;
  out.lastHttp = 0;
  out.lastDetail[0] = 0;

  if (!workspaceId || !workspaceId[0] || !cookie || !cookie[0] || !serverId || !serverId[0]) {
    out.lastError = ocErrNoToken;
    Serial.println("oc: workspace_id / server_id / cookie 不全");
    return false;
  }

  // args 是 JSON，整体 URL 编码后作为查询参数
  char argsJson[192];
  int n = snprintf(argsJson, sizeof(argsJson),
                   "{\"t\":{\"t\":9,\"i\":0,\"l\":1,\"a\":[{\"t\":1,\"s\":\"%s\"}],\"o\":0},"
                   "\"f\":31,\"m\":[]}", workspaceId);
  if (n < 0 || n >= (int)sizeof(argsJson)) {
    out.lastError = ocErrParse;
    strlcpy(out.lastDetail, "workspace_id 过长", sizeof(out.lastDetail));
    return false;
  }

  char path[768];
  int head = snprintf(path, sizeof(path), "/_server?id=%s&args=", serverId);
  if (head < 0 || head >= (int)sizeof(path)) {
    out.lastError = ocErrParse;
    strlcpy(out.lastDetail, "server_id 过长", sizeof(out.lastDetail));
    return false;
  }
  size_t pos = (size_t)head;
  if (!encAppend(path, sizeof(path), pos, argsJson)) {
    out.lastError = ocErrParse;
    strlcpy(out.lastDetail, "url 过长", sizeof(out.lastDetail));
    return false;
  }

  char referer[128];
  snprintf(referer, sizeof(referer), "https://opencode.ai/workspace/%s/go", workspaceId);

  HttpHeader h[] = {
    {"User-Agent",        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"},
    {"X-Server-Id",       serverId},
    {"X-Server-Instance", "server-fn:0"},
    {"Referer",           referer},
    {"Cookie",            cookie},
  };

  String body;
  int st = httpGetSecure("oc", OC_HOST, path, h, sizeof(h) / sizeof(h[0]), body);
  if (st == 0) { out.lastError = ocErrNetwork; return false; }
  if (st == 302 || st == 401 || st == 403) {
    out.lastError = ocErrAuth;
    out.lastHttp = st;
    return false;
  }
  if (st != 200) { out.lastError = ocErrHttp; out.lastHttp = st; return false; }

  OpenCodeUsage tmp = {};
  int found = 0;
  if (scanSegment(body, "rollingUsage:", tmp.rolling)) found++;
  if (scanSegment(body, "weeklyUsage:",  tmp.weekly))  found++;
  if (scanSegment(body, "monthlyUsage:", tmp.monthly)) found++;

  if (found == 0) {
    // cookie 失效时是 HTTP 200 + 一个跳登录页的 302 Response 描述，不是 401
    if (body.indexOf("console/login") >= 0) {
      out.lastError = ocErrAuth;
      Serial.println("oc: cookie 已过期（正文为登录跳转）");
    } else {
      out.lastError = ocErrParse;
      strlcpy(out.lastDetail, "未找到用量字段", sizeof(out.lastDetail));
      Serial.printf("oc: 解析失败，正文 %d 字节\n", body.length());
    }
    return false;
  }

  tmp.valid = true;
  tmp.fetchedAt = millis();
  out = tmp;
  Serial.printf("oc: rolling=%.1f%% weekly=%.1f%% monthly=%.1f%%\n",
                tmp.rolling.percent, tmp.weekly.percent, tmp.monthly.percent);
  return true;
}
