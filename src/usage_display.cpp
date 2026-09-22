#include "usage_display.h"
#include "hw/display.h"
#include <cstdio>
#include <math.h>
#include <time.h>
#include <U8g2lib.h>

#define spr (*hwCanvas())

// ── 设计规范配色：AMOLED 纯黑 + 深灰卡片 + 蓝/青品牌色 ──
static const uint16_t COL_BG     = 0x0000;  // #000000
static const uint16_t COL_CARD   = 0x1083;  // #101018
static const uint16_t COL_TEXT   = 0xF7BF;  // #F5F7FA
static const uint16_t COL_TEXT2  = 0x8C73;  // #8A8F9B
static const uint16_t COL_BLUE   = 0x4C7F;  // #4C8DFF DeepSeek 主色
static const uint16_t COL_CYAN   = 0x6DFF;  // #00D9FF
static const uint16_t COL_GREEN  = 0x4F96;  // #4FF0B0 亮绿（深底卡片上才清晰）
static const uint16_t COL_RED    = 0xF800;

// 画布 184×224（2× 放大到物理 480×480）；边距 4 = 物理 8px，卡片几乎铺满
static const int UI_M    = 4;
static const int UI_CW   = 176;  // 184 - 2×4
static const int UI_R    = 8;

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

static void drawWalletIcon(int x, int y, int s, uint16_t color) {
  spr.drawRoundRect(x, y, s, s * 3 / 4, 3, color);
  // 提手弧
  for (int a = 0; a <= 180; a += 4) {
    float rad = a * 3.14159f / 180.0f;
    spr.drawPixel(x + s / 2 + (int)(s / 5.0f * cosf(rad)),
                  y - (int)(s / 5.0f * sinf(rad)), color);
  }
  spr.drawCircle(x + s * 3 / 4, y + s * 3 / 8, 1, color);
}

// 装饰性上升趋势折线
static void drawTrend(int x, int y) {
  static const int8_t px[5] = { 0, 5, 9, 14, 18 };
  static const int8_t py[5] = { 14, 10, 11, 4, 0 };
  for (int i = 0; i < 4; i++)
    spr.drawLine(x + px[i], y + py[i], x + px[i + 1], y + py[i + 1], COL_BLUE);
  spr.fillCircle(x + px[4], y + py[4], 2, COL_CYAN);
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

void usageDisplayDrawDs(const DeepSeekUsage& data, const char* ip) {
  spr.fillScreen(COL_BG);

  char buf[24];

  // ── Header：logo + 品牌 + 时间 ──
  spr.fillCircle(UI_M + 6, 15, 5, COL_BLUE);
  spr.setFont(u8g2_font_wqy16_t_gb2312b);
  spr.setTextColor(COL_TEXT);
  spr.setCursor(UI_M + 18, 20);
  spr.print("DeepSeek");
  char hm[8];
  formatHm(hm, sizeof(hm));
  if (hm[0]) {
    spr.setFont(u8g2_font_wqy12_t_gb2312b);
    spr.setTextColor(COL_TEXT2);
    spr.setCursor(UI_M + UI_CW - 34, 17);
    spr.print(hm);
  }
  spr.setFont(u8g2_font_wqy12_t_gb2312b);
  spr.setTextColor(COL_TEXT2);
  spr.setCursor(UI_M, 36);
  spr.print("今日用量");

  if (!data.valid) {
    const char* reason;
    switch (data.lastError) {
      case dsErrNoToken: reason = "未配置 token"; break;
      case dsErrNoTime:  reason = "时间未同步"; break;
      case dsErrNetwork: reason = "网络连接失败"; break;
      case dsErrHttp: {
        char tmp[24];
        snprintf(tmp, sizeof(tmp), "HTTP %d", data.lastHttp);
        drawGradCard(84, 96, 0x220D, 0x0883);
        spr.setFont(u8g2_font_wqy16_t_gb2312b);
        spr.setTextColor(COL_RED);
        spr.setCursor(UI_M + 16, 112);
        spr.print("获取失败");
        spr.setFont(u8g2_font_wqy12_t_gb2312b);
        spr.setTextColor(COL_TEXT2);
        spr.setCursor(UI_M + 16, 136);
        spr.print(tmp);
        spr.setCursor(UI_M + 16, 156);
        spr.print("30 秒后自动重试");
        drawStatusBar(ip);
        return;
      }
      default: reason = "数据解析失败"; break;
    }
    drawGradCard(84, 96, 0x220D, 0x0883);
    spr.setFont(u8g2_font_wqy16_t_gb2312b);
    spr.setTextColor(COL_RED);
    spr.setCursor(UI_M + 16, 112);
    spr.print("获取失败");
    spr.setFont(u8g2_font_wqy12_t_gb2312b);
    spr.setTextColor(COL_TEXT2);
    spr.setCursor(UI_M + 16, 136);
    spr.print(reason);
    if (data.lastDetail[0]) {
      spr.setCursor(UI_M + 16, 156);
      spr.print(data.lastDetail);
    }
    spr.setCursor(UI_M + 16, 172);
    spr.print("30 秒后自动重试");
    drawStatusBar(ip);
    return;
  }

  // ── 卡片 1：今日请求数 ──
  spr.fillRoundRect(UI_M, 44, UI_CW, 40, UI_R, COL_CARD);
  spr.setFont(u8g2_font_wqy12_t_gb2312b);
  spr.setTextColor(COL_TEXT2);
  spr.setCursor(UI_M + 12, 56);
  spr.print("今日请求数");
  spr.setFont(u8g2_font_wqy12_t_gb2312b);
  spr.setTextColor(COL_TEXT);
  spr.setTextSize(2);
  spr.setCursor(UI_M + 12, 80);
  snprintf(buf, sizeof(buf), "%lu 次", (unsigned long)data.requestCount);
  spr.print(buf);
  spr.setTextSize(1);
  drawTrend(UI_M + UI_CW - 28, 50);

  // ── 卡片 2：今日 Tokens ──
  spr.fillRoundRect(UI_M, 92, UI_CW, 40, UI_R, COL_CARD);
  spr.setFont(u8g2_font_wqy12_t_gb2312b);
  spr.setTextColor(COL_TEXT2);
  spr.setCursor(UI_M + 12, 104);
  spr.print("今日 Tokens");
  spr.setFont(u8g2_font_wqy12_t_gb2312b);
  spr.setTextColor(COL_TEXT);
  spr.setCursor(UI_M + 12, 128);
  formatCount(buf, sizeof(buf), data.tokenTotal);
  // 数字超过 11 字符（2 倍字号 132px）时降为 1 倍字号防溢出
  spr.setTextSize(strlen(buf) <= 11 ? 2 : 1);
  spr.print(buf);
  spr.setTextSize(1);
  drawTrend(UI_M + UI_CW - 28, 98);

  // ── 卡片 3：账户余额（视觉重点）──
  drawGradCard(148, 64, 0x220D, 0x0883);
  spr.setFont(u8g2_font_wqy12_t_gb2312b);
  spr.setTextColor(COL_TEXT2);
  spr.setCursor(UI_M + 12, 164);
  spr.print("账户余额");
  spr.setFont(u8g2_font_wqy16_t_gb2312b);
  // 数字白色大字（最清晰），¥/元 同色；低于 5 元整体变红警示
  spr.setTextColor(data.balance < 5.0 ? COL_RED : COL_TEXT);
  spr.setCursor(UI_M + 12, 204);
  char num[16];
  snprintf(num, sizeof(num), "%.2f", data.balance);
  int nlen = strlen(num);
  if (strcmp(data.currency, "CNY") == 0) {
    spr.setTextSize(1);
    spr.print("¥");  // 货币符号小号，数字+单位尽量大
    spr.setTextSize(nlen <= 7 ? 2 : 1);
    spr.print(num);
    spr.print(" ");
    spr.print("元");
  } else {
    spr.setTextSize(1);
    spr.print("$");
    spr.setTextSize(nlen <= 7 ? 2 : 1);
    spr.print(num);
  }
  spr.setTextSize(1);
  drawWalletIcon(UI_M + UI_CW - 30, 156, 24, COL_CYAN);

  drawStatusBar(ip);
}
