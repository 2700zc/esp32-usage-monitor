#include "usage_display.h"
#include "hw/display.h"
#include "hw/rtc.h"
#include "hw/power.h"
#include <cstdio>
#include <cstdlib>
#include <U8g2lib.h>
#include <LittleFS.h>

#define spr (*hwCanvas())

static const uint16_t COL_BG    = 0x0000;
static const uint16_t COL_TEXT  = 0xFFFF;
static const uint16_t COL_DIM   = 0x4208;
static const uint16_t COL_GREEN = 0x07E0;
static const uint16_t COL_YELLOW= 0xFFE0;
static const uint16_t COL_RED   = 0xF800;

static uint16_t accentColor(int pct) {
  if (pct > 60) return COL_RED;
  if (pct > 30) return COL_YELLOW;
  return COL_GREEN;
}

static void formatTime(char* buf, size_t sz, uint32_t sec) {
  uint32_t d = sec / 86400; sec %= 86400;
  uint32_t h = sec / 3600;  sec %= 3600;
  uint32_t m = sec / 60;
  if (d > 0) {
    snprintf(buf, sz, "%lu天%lu小时%lu分", d, h, m);
  } else if (h > 0) {
    snprintf(buf, sz, "%lu小时%lu分", h, m);
  } else {
    snprintf(buf, sz, "%lu分", m);
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
      spr.print("距离重置:");
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

static const char* s_weekDays[7] = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };

static void drawJavaLogo(int cx, int cy) {
  static uint16_t* s_img = nullptr;
  if (!s_img) {
    File f = LittleFS.open("/java.raw", "r");
    if (!f) return;
    size_t sz = f.size();
    s_img = (uint16_t*)malloc(sz);
    if (!s_img) { f.close(); return; }
    f.read((uint8_t*)s_img, sz);
    f.close();
  }
  int w = 80, h = 80;
  spr.draw16bitRGBBitmap(cx - w/2, cy - h/2, s_img, w, h);
}

void usageDisplayDrawTime(const char* ip, bool timeValid) {
  spr.fillScreen(COL_BG);

  HwTime t;
  hwRtcRead(&t);

  HwBattery bat = hwBattery();

  if (!timeValid) {
    spr.setFont(u8g2_font_wqy16_t_gb2312b);
    spr.setTextColor(COL_DIM);
    spr.setCursor(SAFE_L, 60);
    spr.print("等待网络同步...");
  } else {
    spr.setFont(u8g2_font_wqy16_t_gb2312b);
    spr.setTextColor(COL_TEXT);
    spr.setCursor(SAFE_L, 35);
    spr.printf("%02u:%02u:%02u", t.H, t.M, t.S);

    spr.setFont(u8g2_font_wqy14_t_gb2312b);
    spr.setTextColor(COL_DIM);
    spr.setCursor(SAFE_L, 65);
    spr.printf("%04u-%02u-%02u %s", t.Y, t.Mo, t.D, s_weekDays[t.dow]);

    char pctBuf[32];
    if (bat.charging) {
      snprintf(pctBuf, sizeof(pctBuf), "%d%% 充电中", bat.pct);
    } else if (bat.usbPresent) {
      snprintf(pctBuf, sizeof(pctBuf), "%d%% 已充满", bat.pct);
    } else {
      snprintf(pctBuf, sizeof(pctBuf), "%d%%", bat.pct);
    }
    spr.setTextColor(bat.pct <= 20 ? COL_YELLOW : COL_TEXT);
    spr.setCursor(SAFE_L, 90);
    spr.print(pctBuf);

    drawJavaLogo(SAFE_L + SAFE_W / 2, 145);
  }

  if (ip && ip[0]) {
    spr.setFont(u8g2_font_wqy12_t_gb2312b);
    spr.setTextColor(COL_YELLOW);
    spr.setCursor(SAFE_L, 210);
    spr.printf("IP: %s", ip);
  }
}

struct Anim {
  uint32_t frameCount, delayMs, w, h;
  uint16_t* pixels;
};

static Anim s_anim = {0};

static bool loadAnim(const char* path) {
  if (s_anim.pixels) return true;
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  uint32_t hdr[4];
  f.read((uint8_t*)hdr, 16);
  size_t fb = hdr[2] * hdr[3] * 2;
  size_t total = fb * hdr[0];
  uint16_t* buf = (uint16_t*)ps_malloc(total);
  if (!buf) { f.close(); return false; }
  f.read((uint8_t*)buf, total);
  f.close();
  s_anim.frameCount = hdr[0];
  s_anim.delayMs     = hdr[1];
  s_anim.w           = hdr[2];
  s_anim.h           = hdr[3];
  s_anim.pixels      = buf;
  return true;
}

static void drawAnimFrame(int cx, int cy, uint32_t frame) {
  if (!s_anim.pixels) return;
  frame %= s_anim.frameCount;
  size_t fb = s_anim.w * s_anim.h;
  spr.draw16bitRGBBitmap(cx - s_anim.w / 2, cy - s_anim.h / 2,
                         s_anim.pixels + frame * fb, s_anim.w, s_anim.h);
}

void usageDisplayDrawThinking(uint32_t animStartMs) {
  loadAnim("/think.anim");

  spr.fillScreen(COL_BG);

  uint32_t elapsed = millis() - animStartMs;
  uint32_t frame = s_anim.delayMs ? (elapsed / s_anim.delayMs) : 0;

  int cx = SAFE_L + SAFE_W / 2;
  int cy = SAFE_T + SAFE_H / 2;
  drawAnimFrame(cx, cy, frame);

  spr.setFont(u8g2_font_wqy16_t_gb2312b);
  spr.setTextColor(COL_TEXT);
  spr.setCursor(cx - 48, cy + s_anim.h / 2 + 30);
  spr.print("思考中...");
}
