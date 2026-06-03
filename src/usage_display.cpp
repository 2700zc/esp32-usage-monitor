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

static void drawIndeterminateBar(int x, int y, int w, uint32_t elapsedMs) {
  int barW = w * 30 / 100;
  uint32_t cycle = elapsedMs % 1000;
  int pos = (int)(((int64_t)cycle * (w + barW)) / 1000) - barW;
  int drawX = pos;
  if (drawX < 0) drawX = 0;
  if (drawX > w - barW) drawX = w - barW;
  spr.fillRect(x, y, w, 4, COL_DIM);
  spr.fillRect(x + drawX, y, barW, 4, COL_GREEN);
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

void usageDisplayDrawDone(int steps, uint32_t elapsedMs) {
  spr.fillScreen(COL_BG);

  // 大号绿色对勾
  spr.setFont(u8g2_font_wqy16_t_gb2312b);
  spr.setTextColor(COL_GREEN);
  int cx = SAFE_L + SAFE_W / 2;
  spr.setCursor(cx - 12, SAFE_T + 60);
  spr.print("OK");

  // "思考完成" 大字
  spr.setCursor(cx - 40, SAFE_T + 100);
  spr.print("思考完成");

  // 统计信息
  spr.setFont(u8g2_font_wqy14_t_gb2312b);
  spr.setTextColor(COL_DIM);
  spr.setCursor(cx - 50, SAFE_T + 135);
  uint32_t sec = elapsedMs / 1000;
  if (sec < 60) {
    spr.printf("%d 步 · 用时 %us", steps, (unsigned int)sec);
  } else if (sec < 3600) {
    spr.printf("%d 步 · 用时 %um%us", steps, (unsigned int)(sec / 60), (unsigned int)(sec % 60));
  } else {
    spr.printf("%d 步 · 用时 %uh%um", steps, (unsigned int)(sec / 3600), (unsigned int)((sec % 3600) / 60));
  }

  // 倒计时提示
  spr.setCursor(cx - 40, SAFE_T + 165);
  spr.printf("%us 后返回...", (unsigned int)(2 - elapsedMs / 1000));
}

void usageDisplayDrawFailed(uint32_t elapsedMs) {
  spr.fillScreen(COL_BG);

  // 红色叉号
  spr.setFont(u8g2_font_wqy16_t_gb2312b);
  spr.setTextColor(COL_RED);
  int cx = SAFE_L + SAFE_W / 2;
  spr.setCursor(cx - 12, SAFE_T + 80);
  spr.print("X");

  // "任务失败" 大字
  spr.setCursor(cx - 40, SAFE_T + 120);
  spr.print("任务失败");

  // 倒计时提示
  spr.setFont(u8g2_font_wqy14_t_gb2312b);
  spr.setTextColor(COL_DIM);
  spr.setCursor(cx - 40, SAFE_T + 155);
  spr.printf("%us 后返回...", (unsigned int)(2 - elapsedMs / 1000));
}

static const char* s_weekDays[7] = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };

static uint16_t* s_img555 = nullptr;
static int s_img555W = 0, s_img555H = 0;

static bool loadImg555() {
  if (s_img555) return true;
  File f = LittleFS.open("/555.raw", "r");
  if (!f) return false;
  size_t sz = f.size();
  s_img555 = (uint16_t*)malloc(sz);
  if (!s_img555) { f.close(); return false; }
  f.read((uint8_t*)s_img555, sz);
  f.close();
  s_img555W = 160;
  s_img555H = 26;
  return true;
}

static void drawLogo555(int cx, int cy) {
  if (!loadImg555()) return;
  spr.draw16bitRGBBitmap(cx - s_img555W / 2, cy - s_img555H / 2, s_img555, s_img555W, s_img555H);
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

    drawLogo555(SAFE_L + SAFE_W / 2, SAFE_T + SAFE_H - 40);
  }

  if (ip && ip[0]) {
    spr.setFont(u8g2_font_wqy12_t_gb2312b);
    spr.setTextColor(COL_YELLOW);
    spr.setCursor(SAFE_L, 210);
    spr.printf("IP: %s", ip);
  }
}

void usageDisplayDrawEaster555(uint32_t elapsedMs) {
  if (!loadImg555()) { spr.fillScreen(COL_BG); return; }
  spr.fillScreen(COL_BG);
  uint32_t cycle = elapsedMs % 200;
  if (cycle < 100) {
    int cx = SAFE_L + SAFE_W / 2;
    int cy = SAFE_T + SAFE_H / 2;
    spr.draw16bitRGBBitmap(cx - s_img555W / 2, cy - s_img555H / 2,
                           s_img555, s_img555W, s_img555H);
  }
}

void usageDisplayDrawThinking(uint32_t elapsedMs, int step, const char* msg) {
  if (!loadImg555()) { spr.fillScreen(COL_BG); return; }
  spr.fillScreen(COL_BG);

  // 555 Logo 闪烁 (200ms 周期, 50% 占空比)
  uint32_t logoCycle = elapsedMs % 200;
  if (logoCycle < 100) {
    int cx = SAFE_L + SAFE_W / 2;
    spr.draw16bitRGBBitmap(cx - s_img555W / 2, SAFE_T + 24,
                           s_img555, s_img555W, s_img555H);
  }

  // "思考中" 文字 + 省略号动画 (600ms 周期: 3/4/5个点)
  spr.setFont(u8g2_font_wqy16_t_gb2312b);
  spr.setTextColor(COL_TEXT);
  spr.setCursor(SAFE_L + 20, SAFE_T + 90);
  uint32_t dotCycle = (elapsedMs % 600) / 200;
  spr.print("思考中");
  for (uint32_t i = 0; i <= dotCycle; i++) spr.print(".");

  // 不定长滚动进度条
  drawIndeterminateBar(SAFE_L + 8, SAFE_T + 108, SAFE_W - 16, elapsedMs);

  // 步骤计数
  spr.setFont(u8g2_font_wqy14_t_gb2312b);
  spr.setTextColor(COL_TEXT);
  spr.setCursor(SAFE_L + 8, SAFE_T + 130);
  spr.printf("步骤 %d", step);

  // 操作描述 (如果有)
  if (msg && msg[0]) {
    spr.setFont(u8g2_font_wqy12_t_gb2312b);
    spr.setTextColor(COL_DIM);
    spr.setCursor(SAFE_L + 8, SAFE_T + 150);
    char truncated[32];
    snprintf(truncated, sizeof(truncated), "%.28s", msg);
    spr.print(truncated);
  }

  // 实时计时
  spr.setFont(u8g2_font_wqy14_t_gb2312b);
  spr.setTextColor(COL_DIM);
  spr.setCursor(SAFE_L + 8, SAFE_T + 172);
  uint32_t sec = elapsedMs / 1000;
  if (sec < 60) {
    spr.printf("已用: %us", (unsigned int)sec);
  } else if (sec < 3600) {
    spr.printf("已用: %um%us", (unsigned int)(sec / 60), (unsigned int)(sec % 60));
  } else {
    spr.printf("已用: %uh%um", (unsigned int)(sec / 3600), (unsigned int)((sec % 3600) / 60));
  }
}

void usageDisplayDrawSttRecording(uint32_t elapsedMs) {
  spr.fillScreen(COL_BG);

  spr.setFont(u8g2_font_wqy16_t_gb2312b);
  spr.setTextColor(COL_RED);
  int cx = SAFE_L + SAFE_W / 2;
  spr.setCursor(cx - 36, SAFE_T + 50);
  spr.print("录音中");

  uint32_t dotCycle = (elapsedMs % 600) / 200;
  for (uint32_t i = 0; i <= dotCycle; i++) spr.print(".");

  spr.setFont(u8g2_font_wqy14_t_gb2312b);
  spr.setTextColor(COL_DIM);
  spr.setCursor(cx - 30, SAFE_T + 85);
  uint32_t sec = elapsedMs / 1000;
  spr.printf("%lus / 10s", (unsigned long)sec);

  drawIndeterminateBar(SAFE_L + 8, SAFE_T + 105, SAFE_W - 16, elapsedMs);

  spr.setFont(u8g2_font_wqy12_t_gb2312b);
  spr.setTextColor(COL_DIM);
  spr.setCursor(SAFE_L + 8, SAFE_T + 140);
  spr.print("按 KEY2 停止录音");
}

void usageDisplayDrawSttUploading() {
  spr.fillScreen(COL_BG);

  spr.setFont(u8g2_font_wqy16_t_gb2312b);
  spr.setTextColor(COL_YELLOW);
  int cx = SAFE_L + SAFE_W / 2;
  spr.setCursor(cx - 50, SAFE_T + 80);
  spr.print("上传识别中...");

  drawIndeterminateBar(SAFE_L + 8, SAFE_T + 105, SAFE_W - 16, millis());
}

void usageDisplayDrawSttResult(const char* text, uint32_t elapsedMs) {
  spr.fillScreen(COL_BG);

  spr.setFont(u8g2_font_wqy16_t_gb2312b);
  spr.setTextColor(COL_GREEN);
  int cx = SAFE_L + SAFE_W / 2;
  spr.setCursor(cx - 36, SAFE_T + 30);
  spr.print("识别结果");

  spr.setFont(u8g2_font_wqy14_t_gb2312b);
  spr.setTextColor(COL_TEXT);

  int y = SAFE_T + 60;
  size_t len = strlen(text);
  const int charsPerLine = 10;
  int lineCount = 0;
  for (size_t i = 0; i < len && lineCount < 8; ) {
    int lineLen = 0;
    int bytePos = 0;
    while (i + bytePos < len && lineLen < charsPerLine) {
      unsigned char c = text[i + bytePos];
      if (c < 0x80) {
        bytePos += 1;
        lineLen += 1;
      } else if ((c & 0xE0) == 0xC0) {
        bytePos += 2;
        lineLen += 2;
      } else if ((c & 0xF0) == 0xE0) {
        bytePos += 3;
        lineLen += 2;
      } else {
        bytePos += 4;
        lineLen += 2;
      }
    }
    char lineBuf[32] = {0};
    int copyLen = bytePos < 31 ? bytePos : 31;
    memcpy(lineBuf, text + i, copyLen);
    lineBuf[copyLen] = '\0';
    spr.setCursor(SAFE_L + 4, y);
    spr.print(lineBuf);
    i += bytePos;
    y += 20;
    lineCount++;
  }

  spr.setFont(u8g2_font_wqy12_t_gb2312b);
  spr.setTextColor(COL_DIM);
  spr.setCursor(SAFE_L + 8, SAFE_T + SAFE_H - 20);
  spr.printf("%us 后返回...", (unsigned int)(3 - elapsedMs / 1000));
}

void usageDisplayDrawSttFailed(uint32_t elapsedMs) {
  spr.fillScreen(COL_BG);

  spr.setFont(u8g2_font_wqy16_t_gb2312b);
  spr.setTextColor(COL_RED);
  int cx = SAFE_L + SAFE_W / 2;
  spr.setCursor(cx - 36, SAFE_T + 80);
  spr.print("识别失败");

  spr.setFont(u8g2_font_wqy14_t_gb2312b);
  spr.setTextColor(COL_DIM);
  spr.setCursor(cx - 40, SAFE_T + 115);
  spr.printf("%us 后返回...", (unsigned int)(3 - elapsedMs / 1000));
}
