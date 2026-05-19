#include "usage_display.h"
#include "hw/display.h"
#include <cstdio>
#include <U8g2lib.h>

#define spr (*hwCanvas())

static const uint16_t COL_BG    = 0x0000;
static const uint16_t COL_TEXT  = 0xFFFF;
static const uint16_t COL_DIM   = 0x4208;
static const uint16_t COL_GREEN = 0x07E0;
static const uint16_t COL_YELLOW= 0xFFE0;
static const uint16_t COL_RED   = 0xF800;

static uint16_t accentColor(int pct) {
  if (pct > 80) return COL_RED;
  if (pct > 10) return COL_YELLOW;
  return COL_GREEN;
}

static void formatTime(char* buf, size_t sz, uint32_t sec) {
  uint32_t d = sec / 86400; sec %= 86400;
  uint32_t h = sec / 3600;  sec %= 3600;
  uint32_t m = sec / 60;    sec %= 60;
  if (d > 0) {
    snprintf(buf, sz, "%lu天%lu小时%lu分%lu秒", d, h, m, sec);
  } else if (h > 0) {
    snprintf(buf, sz, "%lu小时%lu分%lu秒", h, m, sec);
  } else if (m > 0) {
    snprintf(buf, sz, "%lu分%lu秒", m, sec);
  } else {
    snprintf(buf, sz, "%lu秒", sec);
  }
}

static void drawBar(int x, int y, int w, int pct) {
  spr.drawRect(x, y, w, 12, COL_DIM);
  if (pct > 0) {
    int fillW = (w - 2) * pct / 100;
    if (fillW < 0) fillW = 0;
    if (fillW > w - 2) fillW = w - 2;
    spr.fillRect(x + 1, y + 1, fillW, 10, accentColor(pct));
  }
}

void usageDisplayDraw(const UsageData& data, const char* ip) {
  spr.fillScreen(COL_BG);

  spr.setFont(u8g2_font_wqy12_t_gb2312b);

  struct Section {
    const char* label;
    int pct;
    uint32_t resetSec;
  } sections[3] = {
    { "5小时用量",  data.rollingPercent,  data.rollingResetSec },
    { "每周用量",  data.weeklyPercent,   data.weeklyResetSec },
    { "每月用量",  data.monthlyPercent,  data.monthlyResetSec },
  };

  int labelY[] = { 30, 90, 150 };
  int barY[]   = { 46, 106, 166 };
  int resetY[] = { 68, 128, 188 };

  for (int i = 0; i < 3; i++) {
    const auto& sec = sections[i];

    spr.setTextColor(COL_TEXT);
    spr.setCursor(SAFE_L, labelY[i]);
    spr.print(sec.label);

    int pctVal = sec.pct;
    if (!data.valid) pctVal = -1;

    spr.setCursor(SAFE_R - 28, labelY[i]);
    if (pctVal < 0) {
      spr.print("N/A");
    } else {
      spr.printf("%d%%", pctVal);
    }

    drawBar(SAFE_L, barY[i], SAFE_W, data.valid ? pctVal : 0);

    spr.setTextColor(COL_DIM);
    spr.setCursor(SAFE_L, resetY[i]);
    if (!data.valid) {
      spr.print("暂无数据");
    } else if (sec.resetSec == 0) {
      spr.print("即将重置");
    } else {
      spr.print("距离重置时间:");
      char buf[24];
      formatTime(buf, sizeof(buf), sec.resetSec);
      spr.print(buf);
    }
  }

  if (ip && ip[0]) {
    spr.setTextColor(COL_YELLOW);
    spr.setCursor(SAFE_L, 200);
    spr.printf("IP: %s", ip);
  }
}
