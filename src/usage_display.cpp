#include "usage_display.h"
#include "hw/display.h"
#include <Arduino.h>
#include <cstdio>
#include <math.h>
#include <string.h>
#include <time.h>
#include <U8g2lib.h>

#define spr (*hwCanvas())

// ── 设计规范配色：AMOLED 纯黑 + 深灰卡片 + 品牌色 ──
static const uint16_t COL_BG      = 0x0000;  // #000000
static const uint16_t COL_CARD    = 0x1083;  // #101018
static const uint16_t COL_TEXT    = 0xF7BF;  // #F5F7FA
static const uint16_t COL_TEXT2   = 0x8C73;  // #8A8F9B
static const uint16_t COL_BLUE    = 0x4C7F;  // #4C8DFF DeepSeek 主色
static const uint16_t COL_RED     = 0xF800;  // 警示色 / 获取失败
static const uint16_t COL_AMBER   = 0xFD20;  // #FFA020 数据已过期
static const uint16_t COL_DOT_OFF = 0x52AA;  // #555555 未选中的页码点
// TokenHub 里三个服务商的强调色，转 RGB565
static const uint16_t COL_OC      = 0x1BBA;  // #1976d2 OpenCode Go
static const uint16_t COL_CC      = 0x044F;  // #00897b CommandCode
static const uint16_t COL_AGENT   = 0x8AFE;  // #8B5CF6 Agent 状态页
static const uint16_t COL_GREEN   = 0x2E6E;  // #2ECC71 思考完成
static const uint16_t COL_SEND    = 0xFFE0;  // #FFE000 按键页的 Enter
static const uint16_t COL_ESC     = 0xFD20;  // #FFA020 按键页的 Esc

// 画布 184×224（2× 放大到物理 480×480）；边距 4 = 物理 8px，卡片几乎铺满
static const int UI_M  = 4;
static const int UI_CW = 176;  // 184 - 2×4
static const int UI_R  = 8;

// ── 行高来自固件二进制里读出的 u8g2 真实度量，不是估算 ──
//   wqy12 size1: 基线以上 11px / 以下 2px -> 13px
//   wqy12 size2: 基线以上 22px / 以下 4px -> 26px
//   wqy16 size1: 基线以上 14px / 以下 4px -> 18px
//   wqy16 size2: 基线以上 28px / 以下 8px -> 36px
// 数值用 wqy12 size2（26px），只保留两行，基线写死如下。
static const int CARD_Y[3] = { 42, 96, 150 };
static const int CARD_H    = 52;
static const int ROW1_BASE = 17;  // 标签 + 右侧次要信息：占 y+6..y+19
static const int ROW2_BASE = 47;  // 数值：占 y+25..y+51
static const int VALUE_MAX_W = UI_CW - 16;  // 数值可用宽度（左 10 + 右 6 余量）

// ── 通用绘制件 ──

static uint16_t lerpColor(uint16_t c1, uint16_t c2, float t) {
  if (t <= 0) return c1;
  if (t >= 1) return c2;
  int r1 = (c1 >> 11) & 31, g1 = (c1 >> 5) & 63, b1 = c1 & 31;
  int r2 = (c2 >> 11) & 31, g2 = (c2 >> 5) & 63, b2 = c2 & 31;
  int r = r1 + (int)((r2 - r1) * t);
  int g = g1 + (int)((g2 - g1) * t);
  int b = b1 + (int)((b2 - b1) * t);
  return (r << 11) | (g << 5) | b;
}

// 圆角对角线渐变卡片：左上亮 → 右下暗，圆角区域保留背景色
static void drawGradCard(int y, int h, uint16_t c1, uint16_t c2) {
  int r = UI_R;
  for (int i = 0; i < h; i++) {
    int x0 = UI_M, x1 = UI_M + UI_CW;
    int dy = i < r ? r - i : (i > h - 1 - r ? i - (h - 1 - r) : 0);
    if (dy > 0) {
      int dx = (int)sqrtf((float)(r * r - dy * dy));
      x0 += dx;
      x1 -= dx;
    }
    if (x1 <= x0) continue;
    float t0 = (float)i / (h - 1);
    float t1 = (float)((x1 - 1 - UI_M) + i) / (UI_CW - 1 + h - 1);
    uint16_t cStart = lerpColor(c1, c2, t0);
    uint16_t cEnd   = lerpColor(c1, c2, t1);
    for (int x = x0; x < x1; x++) {
      float t = (float)(x - x0) / (x1 - 1 - x0);
      spr.drawPixel(x, y + i, lerpColor(cStart, cEnd, t));
    }
  }
}

// 1/4 圆弧（顶部 120°），用于 WiFi 图标
static void drawArc(int cx, int cy, int r, uint16_t color) {
  for (int a = -60; a <= 60; a += 2) {
    float rad = a * 3.14159f / 180.0f;
    spr.drawPixel(cx + (int)(r * sinf(rad)), cy - (int)(r * cosf(rad)), color);
  }
}

static void drawWifiIcon(int x, int y) {
  drawArc(x + 7, y + 6, 6, COL_BLUE);
  drawArc(x + 7, y + 6, 3, COL_BLUE);
  spr.fillCircle(x + 7, y + 6, 1, COL_BLUE);
}

// 完整数字 + 千分位；超过 15 字符（含逗号）退回"亿"缩写防溢出
static void formatCount(char* buf, size_t sz, uint64_t v) {
  char tmp[24];
  snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)v);
  int len = strlen(tmp);
  int groups = (len - 1) / 3;
  if (len + groups >= (int)sz || len + groups > 15) {
    snprintf(buf, sz, "%.2f 亿", v / 100000000.0);
    return;
  }
  int si = 0, oi = 0;
  while (si < len) {
    if (oi > 0 && (len - si) % 3 == 0) buf[oi++] = ',';
    buf[oi++] = tmp[si++];
  }
  buf[oi] = 0;
}

static void formatHm(char* buf, size_t sz) {
  time_t ts = time(nullptr);
  if (ts < 100000) {
    buf[0] = 0;
    return;
  }
  struct tm lt;
  localtime_r(&ts, &lt);
  snprintf(buf, sz, "%02d:%02d", lt.tm_hour, lt.tm_min);
}

// 距上次成功获取过去了多久。用相对时长而不是绝对时刻：设备没同步 NTP 时绝对
// 时刻会显示成 1970，而"多久之前"也更能一眼看出数据有多旧。
static void formatAge(char* buf, size_t sz, uint32_t fetchedAtMs) {
  if (fetchedAtMs == 0) { strlcpy(buf, "--", sz); return; }
  uint32_t sec = (millis() - fetchedAtMs) / 1000;
  if (sec < 90)        strlcpy(buf, "刚刚", sz);
  else if (sec < 3600) snprintf(buf, sz, "%lu 分钟前", (unsigned long)(sec / 60));
  else                 snprintf(buf, sz, "%lu 小时前", (unsigned long)(sec / 3600));
}

// u8g2 的 wqy 系列是定宽字体：ASCII 前进 = cell/2，CJK（含 ¥ 与 ·） = cell。
// 直接量宽度比按字符数猜可靠——中英混排时字符数会明显低估。
static int textWidth(const char* s, int cell, int size) {
  int w = 0;
  for (const unsigned char* p = (const unsigned char*)s; *p; ) {
    if (*p < 0x80)               { w += cell / 2; p += 1; }
    else if ((*p & 0xE0) == 0xC0) { w += cell;     p += 2; }
    else if ((*p & 0xF0) == 0xE0) { w += cell;     p += 3; }
    else                          { w += cell;     p += 4; }
  }
  return w * size;
}

// 底部状态栏：WiFi 图标 + IP + 时间
static void drawStatusBar(const char* ip) {
  char hm[8];
  formatHm(hm, sizeof(hm));
  drawWifiIcon(UI_M, 216);
  spr.setFont(u8g2_font_wqy12_t_gb2312b);
  spr.setTextColor(COL_TEXT2);
  spr.setCursor(UI_M + 20, 222);
  if (ip && ip[0]) spr.print(ip);
  if (hm[0]) {
    spr.setCursor(UI_M + UI_CW - 34, 222);
    spr.print(hm);
  }
}

// 页码圆点，当前页高亮。单页不画。
static void drawPageDots(int page, int count) {
  if (count <= 1) return;
  const int pitch = 7, r = 2, y = 208;
  int width = (count - 1) * pitch + (2 * r + 1);
  int x0 = (HW_W - width) / 2 + r;
  for (int i = 0; i < count; i++)
    spr.fillCircle(x0 + i * pitch, y, r, i == page ? COL_TEXT : COL_DOT_OFF);
}

// 页头：品牌点 + 服务商名 + 右上时间 + 副标题
static void drawHeader(const char* name, uint16_t accent, const char* subtitle, uint16_t subColor) {
  spr.fillCircle(UI_M + 6, 15, 5, accent);
  spr.setFont(u8g2_font_wqy16_t_gb2312b);
  spr.setTextColor(COL_TEXT);
  spr.setCursor(UI_M + 18, 20);
  spr.print(name);

  char hm[8];
  formatHm(hm, sizeof(hm));
  if (hm[0]) {
    spr.setFont(u8g2_font_wqy12_t_gb2312b);
    spr.setTextColor(COL_TEXT2);
    spr.setCursor(UI_M + UI_CW - 34, 17);
    spr.print(hm);
  }
  spr.setFont(u8g2_font_wqy12_t_gb2312b);
  spr.setTextColor(subColor);
  spr.setCursor(UI_M, 36);
  spr.print(subtitle);
}

// 页头副标题的两种形态：正常时是静态副标题；上次刷新失败时换成琥珀色的
// 「失败原因 · 数据年龄」——页面照常显示历史数据，靠这一行区分新旧。
// 一行只有 174px，放不下「原因 · 年龄」时就只留原因（各原因的宽度已核过，都能放下）。
static void drawHeaderStatus(const char* name, uint16_t accent, const char* subtitle,
                             const char* failReason, uint32_t fetchedAt) {
  const int MAX_W = 174;
  char buf[44];
  const char* text = subtitle;
  uint16_t color = COL_TEXT2;
  if (failReason && fetchedAt != 0) {
    char age[16];
    formatAge(age, sizeof(age), fetchedAt);
    snprintf(buf, sizeof(buf), "%s · %s", failReason, age);
    if (textWidth(buf, 12, 1) > MAX_W) snprintf(buf, sizeof(buf), "%s", failReason);
    text = buf;
    color = COL_AMBER;
  }
  drawHeader(name, accent, text, color);
}

// 整页提示卡片：只在「从没成功获取过」时用。
// 行距按 wqy16 size1（18px）与 wqy12 size1（13px）排，互不重叠。
static void drawStatusBody(const char* title, uint16_t titleColor, const char* reason,
                           const char* detail, const char* ip, int page, int pageCount,
                           const char* hint = "60 秒后自动重试") {
  drawGradCard(76, 104, 0x220D, 0x0883);
  spr.setFont(u8g2_font_wqy16_t_gb2312b);
  spr.setTextColor(titleColor);
  spr.setCursor(UI_M + 16, 106);
  spr.print(title);

  spr.setFont(u8g2_font_wqy12_t_gb2312b);
  spr.setTextColor(COL_TEXT2);
  spr.setCursor(UI_M + 16, 132);
  spr.print(reason);
  if (detail && detail[0]) {
    spr.setCursor(UI_M + 16, 152);
    spr.print(detail);
  }
  spr.setCursor(UI_M + 16, 172);
  spr.print(hint);

  drawPageDots(page, pageCount);
  drawStatusBar(ip);
}

// ── 卡片：第 1 行「标签 + 右侧次要信息」，第 2 行「大号数值」 ──

static void drawCardBase(int y) {
  spr.fillRoundRect(UI_M, y, UI_CW, CARD_H, UI_R, COL_CARD);
}

// 标签行。右侧文本（重置倒计时 / 币种 / 刷新说明）右对齐；若会压到标签则整条不画，
// 宁可少一行也不叠字。
static void drawCardRow1(int y, const char* label, const char* right) {
  spr.setFont(u8g2_font_wqy12_t_gb2312b);
  spr.setTextColor(COL_TEXT2);
  spr.setCursor(UI_M + 10, y + ROW1_BASE);
  spr.print(label);

  if (!right || !right[0]) return;
  int w = textWidth(right, 12, 1);
  int x = UI_M + UI_CW - 10 - w;
  if (x < UI_M + 10 + textWidth(label, 12, 1) + 6) return;
  spr.setCursor(x, y + ROW1_BASE);
  spr.print(right);
}

// 数值行。默认 wqy12 的 2 倍字号；放不下（超过 13 个字符宽）时降回 1 倍，
// 同时把基线压低，否则小字号会浮在行中间。
static void drawCardValue(int y, const char* value, uint16_t color) {
  bool big = textWidth(value, 12, 2) <= VALUE_MAX_W;
  spr.setFont(u8g2_font_wqy12_t_gb2312b);
  spr.setTextSize(big ? 2 : 1);
  spr.setTextColor(color);
  spr.setCursor(UI_M + 10, y + (big ? ROW2_BASE : 40));
  spr.print(value);
  spr.setTextSize(1);
}

// 百分比文案，与 TokenHub 的 _fmt_cc_pct 一致：最多两位小数、去掉尾随的 0
// （0.54% / 3.01% / 25%），到 100% 封顶。
// OpenCode 的额度由微元计价换算而来，小数位很碎，只留一位会把 0.54% 和 0.56%
// 显示成同一个值。
static void fmtPercent(char* buf, size_t sz, float pct) {
  if (pct >= 100.0f) { strlcpy(buf, "100%", sz); return; }
  char num[16];
  snprintf(num, sizeof(num), "%.2f", (double)pct);
  size_t n = strlen(num);
  while (n > 0 && num[n - 1] == '0') num[--n] = 0;  // 3.00 -> "3."
  if (n > 0 && num[n - 1] == '.')    num[--n] = 0;  // "3." -> "3"
  snprintf(buf, sz, "%s%%", num);
}

// 用量百分比卡片。无上限 / 无额度时数值显示 --，原因写在标签行右侧。
static void drawUsageCard(int y, const char* label, const UsageSegment& seg, uint16_t accent) {
  drawCardBase(y);

  char right[24];
  right[0] = 0;
  if (strcmp(seg.status, "unlimited") == 0) {
    strlcpy(right, "无上限", sizeof(right));
  } else if (strcmp(seg.status, "unavailable") == 0) {
    strlcpy(right, "额度不可用", sizeof(right));
  } else if (seg.valid) {
    usageFormatResetShort(right, sizeof(right), seg.resetSec);
  }

  char buf[24];
  if (seg.valid) fmtPercent(buf, sizeof(buf), seg.percent);
  else           strlcpy(buf, "--", sizeof(buf));

  // 达到 100% 用红色警示——TokenHub 无阈值配色，这里按小屏可读性补上
  drawCardValue(y, buf, seg.valid && seg.percent >= 100.0f ? COL_RED : accent);
  drawCardRow1(y, label, right);
}

// ── 三个页面 ──

void usageDisplayDrawOc(const OpenCodeUsage& data, const char* ip, int page, int pageCount,
                        const char* netStatus) {
  spr.fillScreen(COL_BG);

  char httpTxt[16] = "";
  const char* reason = nullptr;   // nullptr = 上一次刷新是成功的
  const char* detail = data.lastDetail;
  switch (data.lastError) {
    case ocErrNoToken: reason = "未配置凭据"; break;
    case ocErrNetwork: reason = "网络连接失败"; break;
    // 具体是 key 被拒还是 cookie 过期，由客户端写进 lastDetail（两种凭据的补救方式不同）
    case ocErrAuth:    reason = "凭据已失效";
                       detail = data.lastDetail[0] ? data.lastDetail
                                                   : "重新获取后重刷文件系统"; break;
    case ocErrNoSession: reason = "cookie 缺少 session";
                         detail = "重填含 session 的 cookie"; break;
    case ocErrHttp:    snprintf(httpTxt, sizeof(httpTxt), "HTTP %d", data.lastHttp);
                       reason = httpTxt; break;
    case ocErrParse:   reason = "数据解析失败"; break;
    default:           break;     // ocErrOk
  }
  if (netStatus) reason = netStatus;

  if (!data.valid) {
    // 从没成功过：整页提示。页头保持静态副标题，原因只写在卡片里，避免重复
    drawHeader("OpenCode Go", COL_OC, "用量窗口", COL_TEXT2);
    drawStatusBody(reason ? "获取失败" : "正在获取", reason ? COL_RED : COL_TEXT2,
                   reason ? reason : "等待首次刷新", detail, ip, page, pageCount);
    return;
  }

  drawHeaderStatus("OpenCode Go", COL_OC, "用量窗口", reason, data.fetchedAt);
  drawUsageCard(CARD_Y[0], "滚动用量", data.rolling, COL_OC);
  drawUsageCard(CARD_Y[1], "每周用量", data.weekly,  COL_OC);
  drawUsageCard(CARD_Y[2], "每月用量", data.monthly, COL_OC);
  drawPageDots(page, pageCount);
  drawStatusBar(ip);
}

void usageDisplayDrawCc(const CommandCodeUsage& data, const char* ip, int page, int pageCount,
                        const char* netStatus) {
  spr.fillScreen(COL_BG);

  char httpTxt[16] = "";
  const char* reason = nullptr;
  const char* detail = data.lastDetail;
  switch (data.lastError) {
    case ccErrNoToken: reason = "未配置凭据"; break;
    case ccErrNetwork: reason = "网络连接失败"; break;
    case ccErrAuth:    reason = "token 已过期";
                       detail = "重新复制后重刷文件系统"; break;
    case ccErrHttp:    snprintf(httpTxt, sizeof(httpTxt), "HTTP %d", data.lastHttp);
                       reason = httpTxt; break;
    case ccErrParse:   reason = "数据解析失败"; break;
    default:           break;     // ccErrOk
  }
  if (netStatus) reason = netStatus;

  if (!data.valid) {
    drawHeader("CommandCode", COL_CC, "用量窗口", COL_TEXT2);
    drawStatusBody(reason ? "获取失败" : "正在获取", reason ? COL_RED : COL_TEXT2,
                   reason ? reason : "等待首次刷新", detail, ip, page, pageCount);
    return;
  }

  drawHeaderStatus("CommandCode", COL_CC, "用量窗口", reason, data.fetchedAt);
  drawUsageCard(CARD_Y[0], "滚动用量", data.fiveHour, COL_CC);
  drawUsageCard(CARD_Y[1], "每周用量", data.weekly,   COL_CC);
  drawUsageCard(CARD_Y[2], "每月用量", data.monthly,  COL_CC);
  drawPageDots(page, pageCount);
  drawStatusBar(ip);
}

// 余额卡片：¥/$ 符号用 1 倍字号、数字尽量大；低于 5 元整体变红警示
static void drawDsBalanceCard(int y, const DeepSeekUsage& data) {
  drawGradCard(y, CARD_H, 0x220D, 0x0883);

  char num[16];
  snprintf(num, sizeof(num), "%.2f", data.balance);
  const char* sym = strcmp(data.currency, "CNY") == 0 ? "¥" : "$";

  // 币种写在右侧，所以不再重复"元"
  drawCardRow1(y, "账户余额", data.currency);

  bool big = textWidth(sym, 12, 1) + textWidth(num, 12, 2) <= VALUE_MAX_W;
  spr.setFont(u8g2_font_wqy12_t_gb2312b);
  spr.setTextColor(data.balance < 5.0 ? COL_RED : COL_TEXT);
  spr.setCursor(UI_M + 10, y + (big ? ROW2_BASE : 40));
  spr.setTextSize(1);
  spr.print(sym);
  spr.setTextSize(big ? 2 : 1);
  spr.print(num);
  spr.setTextSize(1);
}

void usageDisplayDrawDs(const DeepSeekUsage& data, const char* ip, int page, int pageCount,
                        const char* netStatus) {
  spr.fillScreen(COL_BG);

  char httpTxt[16] = "";
  const char* reason = nullptr;
  switch (data.lastError) {
    case dsErrNoToken: reason = "未配置 token"; break;
    case dsErrNoTime:  reason = "时间未同步"; break;
    case dsErrNetwork: reason = "网络连接失败"; break;
    case dsErrHttp:    snprintf(httpTxt, sizeof(httpTxt), "HTTP %d", data.lastHttp);
                       reason = httpTxt; break;
    case dsErrParse:   reason = "数据解析失败"; break;
    default:           break;     // dsErrOk
  }
  if (netStatus) reason = netStatus;

  if (!data.valid) {
    drawHeader("DeepSeek", COL_BLUE, "今日用量", COL_TEXT2);
    drawStatusBody(reason ? "获取失败" : "正在获取", reason ? COL_RED : COL_TEXT2,
                   reason ? reason : "等待首次刷新", data.lastDetail, ip, page, pageCount);
    return;
  }

  drawHeaderStatus("DeepSeek", COL_BLUE, "今日用量", reason, data.fetchedAt);

  char buf[24];
  snprintf(buf, sizeof(buf), "%lu 次", (unsigned long)data.requestCount);
  drawCardBase(CARD_Y[0]);
  drawCardValue(CARD_Y[0], buf, COL_TEXT);
  drawCardRow1(CARD_Y[0], "今日请求数", "每 60 秒刷新");

  formatCount(buf, sizeof(buf), data.tokenTotal);
  drawCardBase(CARD_Y[1]);
  drawCardValue(CARD_Y[1], buf, COL_TEXT);
  drawCardRow1(CARD_Y[1], "今日 Tokens", "缓存+输入+输出");

  drawDsBalanceCard(CARD_Y[2], data);
  drawPageDots(page, pageCount);
  drawStatusBar(ip);
}

// ── Agent 状态页 ──

// 状态 -> 文案。桥接以后加了新状态（或字段缺失）就原样显示，不猜。
static const char* agentStateLabel(const char* state, char* buf, size_t sz) {
  if (strcmp(state, "thinking") == 0) return "正在思考";
  if (strcmp(state, "tool") == 0)     return "执行工具";
  if (strcmp(state, "waiting") == 0)  return "等待授权";
  if (strcmp(state, "done") == 0)     return "思考完成";
  if (strcmp(state, "idle") == 0)     return "空闲";
  snprintf(buf, sz, "%s", state);
  return buf;
}

// 配色与文案同源：等待授权用琥珀（要人管），完成用绿，思考用蓝
static uint16_t agentStateColor(const char* state) {
  if (strcmp(state, "thinking") == 0) return COL_BLUE;
  if (strcmp(state, "tool") == 0)     return COL_CC;
  if (strcmp(state, "waiting") == 0)  return COL_AMBER;
  if (strcmp(state, "done") == 0)     return COL_GREEN;
  return COL_TEXT2;  // 空闲
}

// 秒级新鲜度。formatAge 的"刚刚"覆盖 90 秒，在一个每秒更新的状态页上等于没有
// 信息——这里 90 秒内按秒显示。
static void formatAgeShort(char* buf, size_t sz, uint32_t sec) {
  if (sec < 90)        snprintf(buf, sz, "%lu 秒前", (unsigned long)sec);
  else if (sec < 3600) snprintf(buf, sz, "%lu 分钟前", (unsigned long)(sec / 60));
  else                 snprintf(buf, sz, "%lu 小时前", (unsigned long)(sec / 3600));
}

// 按真实显示宽度裁剪（wqy 是定宽字体：ASCII 占半格、中文占一格），超长时末尾
// 补省略号。桥接已经按字符数截过一次，但中文和 ASCII 的宽度差一倍，只有这里
// 知道实际像素宽度——一个 26 字符的中文串比 26 字符的英文宽一倍，会直接画出卡片。
static void fitText(char* dst, size_t dstSz, const char* src, int maxW) {
  if (textWidth(src, 12, 1) <= maxW) {
    strlcpy(dst, src, dstSz);
    return;
  }
  const int ellW = textWidth("…", 12, 1);
  size_t out = 0;
  dst[0] = 0;
  for (const unsigned char* p = (const unsigned char*)src; *p; ) {
    int cl = (*p < 0x80) ? 1 : ((*p & 0xE0) == 0xC0) ? 2 : ((*p & 0xF0) == 0xE0) ? 3 : 4;
    if (out + cl + 4 >= dstSz) break;
    char probe[80];
    memcpy(probe, dst, out);
    memcpy(probe + out, p, cl);
    probe[out + cl] = 0;
    if (textWidth(probe, 12, 1) + ellW > maxW) break;
    memcpy(dst + out, p, cl);
    out += cl;
    dst[out] = 0;
    p += cl;
  }
  strlcpy(dst + out, "…", dstSz - out);
}

void usageDisplayDrawAgent(const AgentStatus& data, const char* ip, int page, int pageCount,
                           const char* netStatus) {
  spr.fillScreen(COL_BG);

  char httpTxt[16] = "";
  const char* reason = nullptr;
  switch (data.lastError) {
    case agErrBadUrl:  reason = "桥接地址格式不对"; break;
    case agErrNetwork: reason = "连不上桥接"; break;
    case agErrHttp:    snprintf(httpTxt, sizeof(httpTxt), "HTTP %d", data.lastHttp);
                       reason = httpTxt; break;
    case agErrParse:   reason = "数据解析失败"; break;
    default:           break;     // agErrOk，以及搜索中的两种（下面单独处理）
  }
  if (netStatus) reason = netStatus;
  // 桥接自己说这个状态可能过期了（会话进程中途没了、事件断了），按失败处理：
  // 状态照常显示，页头换成琥珀色。与另外三页"失败不清 valid"一致。
  if (!reason && data.valid && data.stale) reason = "状态可能已过期";

  if (!data.valid) {
    // 三种情况要分开说，别都糊成"获取失败"：还在广播找桥接（正常过程）、
    // 找了一分钟没人应答（该去 PC 上看看了）、有地址但取不到（桥接没开/防火墙）
    const char* title;
    const char* sub;
    const char* hint = "自动重试中";
    uint16_t    tcol;
    if (data.lastError == agErrSearching) {
      title = "正在查找桥接"; tcol = COL_TEXT2; sub = "局域网广播中";
    } else if (data.lastError == agErrNotFound) {
      title = "没找到桥接";   tcol = COL_TEXT2; sub = "确认 PC 上 bridge.py 在运行";
      hint  = "每分钟再找一次";
    } else {
      title = reason ? "获取失败" : "正在获取";
      tcol  = reason ? COL_RED : COL_TEXT2;
      sub   = reason ? reason : "等待首次刷新";
    }
    drawHeader("Agent", COL_AGENT, "状态", COL_TEXT2);
    drawStatusBody(title, tcol, sub, data.lastDetail, ip, page, pageCount, hint);
    return;
  }

  char sub[48], age[16];
  formatAgeShort(age, sizeof(age), data.ageSec);
  snprintf(sub, sizeof(sub), "状态 · %s", age);
  drawHeaderStatus("Agent", COL_AGENT, sub, reason, data.fetchedAt);

  // 卡片 1：状态本体。会话数只在多于一个时出现（同时开两个窗口才有意义）
  char label[16];
  drawCardBase(CARD_Y[0]);
  drawCardValue(CARD_Y[0], agentStateLabel(data.state, label, sizeof(label)),
                agentStateColor(data.state));
  char sessions[20];
  sessions[0] = 0;
  if (data.activeSessions > 1) snprintf(sessions, sizeof(sessions), "%u 个会话", data.activeSessions);
  drawCardRow1(CARD_Y[0], "当前状态", sessions);

  // 卡片 2：正在执行的工具。右侧标出这一份是 UDP 推来的还是轮询取的——
  // 两条链路出问题时这是最快的区分办法。
  drawCardBase(CARD_Y[1]);
  drawCardValue(CARD_Y[1], data.tool[0] ? data.tool : "--",
                data.tool[0] ? COL_TEXT : COL_TEXT2);
  drawCardRow1(CARD_Y[1], "当前工具", data.pushed ? "实时" : "轮询");

  // 卡片 3：正在做什么。工具类事件带 action（Bash 的命令、Edit 的文件名），
  // 其余状态退回显示会话所属的项目名——"已持续多久"看两眼就腻了，不如换成
  // 一个能回答"它现在到底在干嘛"的字段。
  const char* c3label = "当前项目";
  const char* c3src   = data.project;
  if ((strcmp(data.state, "tool") == 0 || strcmp(data.state, "waiting") == 0)
      && data.action[0]) {
    c3label = "当前动作";
    c3src   = data.action;
  }
  char fitted[80];
  fitText(fitted, sizeof(fitted), c3src[0] ? c3src : "--", VALUE_MAX_W);
  drawCardBase(CARD_Y[2]);
  drawCardValue(CARD_Y[2], fitted, c3src[0] ? COL_TEXT : COL_TEXT2);
  drawCardRow1(CARD_Y[2], c3label, nullptr);

  drawPageDots(page, pageCount);
  drawStatusBar(ip);
}

void usageDisplayDrawEmpty(const char* ip, bool pagesOff) {
  spr.fillScreen(COL_BG);
  drawHeader("用量监控", COL_TEXT2, pagesOff ? "所有页面已关闭" : "未配置服务商", COL_TEXT2);
  drawGradCard(76, 104, 0x220D, 0x0883);
  spr.setFont(u8g2_font_wqy16_t_gb2312b);
  spr.setTextColor(COL_TEXT);
  spr.setCursor(UI_M + 16, 106);
  spr.print(pagesOff ? "页面都关了" : "还没有凭据");
  spr.setFont(u8g2_font_wqy12_t_gb2312b);
  spr.setTextColor(COL_TEXT2);
  spr.setCursor(UI_M + 16, 134);
  spr.print(pagesOff ? "长按 BOOT" : "编辑 data/config.json");
  spr.setCursor(UI_M + 16, 156);
  spr.print(pagesOff ? "打开页面开关" : "再执行 uploadfs");
  drawPageDots(0, 1);
  drawStatusBar(ip);
}

// ── 按键页（实验性）──

static void drawCentered(const char* s, int y, int size, uint16_t color) {
  spr.setFont(u8g2_font_wqy12_t_gb2312b);
  spr.setTextSize(size);
  spr.setTextColor(color);
  spr.setCursor((HW_W - textWidth(s, 12, size)) / 2, y);
  spr.print(s);
  spr.setTextSize(1);
}

void usageDisplayDrawSend(const AgentStatus& bridge, const SendUiState& ui,
                          const char* ip, int page, int pageCount, const char* netStatus) {
  spr.fillScreen(COL_BG);

  const char* sub;
  uint16_t    subColor;
  if (netStatus) {
    sub = netStatus;                        subColor = COL_RED;
  } else if (bridge.valid && bridge.lastError == agErrOk) {
    sub = "桥接 · 已连接";                  subColor = COL_GREEN;
  } else if (bridge.lastError == agErrSearching) {
    sub = "桥接 · 正在查找";                subColor = COL_TEXT2;
  } else if (bridge.lastError == agErrNotFound) {
    sub = "桥接 · 没找到";                  subColor = COL_AMBER;
  } else {
    sub = "桥接 · 未连接";                  subColor = COL_RED;
  }
  drawHeader("按键", COL_SEND, sub, subColor);

  // 两张说明卡：按键是物理键 IO18，触摸只负责显示。
  drawCardBase(42);
  drawCardValue(42, "回车", COL_SEND);
  drawCardRow1(42, "短按 IO18", "Enter");

  drawCardBase(100);
  drawCardValue(100, "Esc", COL_ESC);
  drawCardRow1(100, "长按 IO18 0.7 秒", "Escape");

  // 最近一次结果。--result = -1 表示这次开机还没按过。
  char line[44];
  uint16_t color = COL_TEXT2;
  if (ui.busy) {
    strlcpy(line, ui.busyAction ? "正在按 Esc…" : "正在发送回车…", sizeof(line));
    color = COL_AMBER;
  } else if (ui.result < 0) {
    strlcpy(line, "还没按过键", sizeof(line));
  } else {
    const char* text;
    switch ((AgActCode)ui.result) {
      case agActOk:
        text = ui.lastAction ? "已按 Esc" : "已发送回车";
        color = COL_GREEN; break;
      case agActNoToken:    text = "未配置 act_token";   color = COL_AMBER; break;
      case agActNoHost:     text = "还没找到桥接";       color = COL_AMBER; break;
      case agActNetwork:    text = "连不上桥接";         color = COL_RED;   break;
      case agActDenied:     text = "令牌不对/未启用";    color = COL_AMBER; break;
      case agActForeground: text = "前台不是 ZCode";     color = COL_AMBER; break;
      case agActNoEdit:     text = "找不到输入框";       color = COL_AMBER; break;
      case agActOldBridge:  text = "桥接版本太旧";       color = COL_AMBER; break;
      default:              text = "发送失败";           color = COL_RED;   break;
    }
    // 这一页按 10 秒兜底重绘，所以只给"分钟"粒度。
    uint32_t sec = (millis() - ui.atMs) / 1000;
    if (sec < 60) snprintf(line, sizeof(line), "%s", text);
    else          snprintf(line, sizeof(line), "%s · %lu分前", text,
                           (unsigned long)(sec / 60));
  }
  drawCentered(line, 172, 1, color);
  drawCentered("物理键在任意页面生效", 190, 1, COL_TEXT2);

  drawPageDots(page, pageCount);
  drawStatusBar(ip);
}
