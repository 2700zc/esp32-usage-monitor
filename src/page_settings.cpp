#include "page_settings.h"
#include "config.h"
#include "hw/display.h"
#include "hw/input.h"
#include <Arduino.h>
#include <Preferences.h>
#include <U8g2lib.h>
#include <stdio.h>

#define spr (*hwCanvas())

// 配色与 usage_display 保持一致：纯黑底 + 深灰圆角卡片 + 各家品牌色
static const uint16_t C_BG    = 0x0000;
static const uint16_t C_CARD  = 0x1083;
static const uint16_t C_SEL   = 0x18C6;
static const uint16_t C_TEXT  = 0xF7BF;
static const uint16_t C_TEXT2 = 0x8C73;
static const uint16_t C_ACC   = 0xFFE0;
static const uint16_t C_WARN  = 0xFD20;

// 画布 184×224，与主界面同一套边距（见 usage_display.cpp 的 UI_M/UI_CW）
static const int UI_M  = 4;
static const int UI_CW = 176;

// 五个页面开关 + 一行「WiFi 配置」+ 一行「完成」。6 行把行高/间距压到
// 21/22：最后一行底边 171，「完成」176..202，提示行 214，224 高的画布放得下。
static const int ROW_Y0    = 40;
static const int ROW_H     = 21;
static const int ROW_PITCH = 22;
static const int ROW_COUNT = 6;
static const int TOGGLE_COUNT = 5;   // 前 5 行是页面开关，最后一行是 WiFi 入口
static const int DONE_SEL  = 6;
static const int DONE_Y    = 176;
static const int DONE_H    = 26;

static const char* PAGES_NVS_NS = "pages";

enum RowKind : uint8_t { ROW_TOGGLE, ROW_WIFI };

struct RowDef {
  const char* label;
  uint16_t    accent;
  uint8_t     bit;    // ROW_WIFI 行为 0，不参与 mask
  RowKind     kind;
};

static const RowDef ROWS[ROW_COUNT] = {
  { "状态页",       0x8AFE, PG_AGENT, ROW_TOGGLE },  // Agent 紫
  { "OpenCode Go",  0x1BBA, PG_OC,    ROW_TOGGLE },  // #1976d2
  { "CommandCode",  0x044F, PG_CC,    ROW_TOGGLE },  // #00897b
  { "DeepSeek",     0x4C7F, PG_DS,    ROW_TOGGLE },  // #4C8DFF
  { "按键页",       0xFFE0, PG_SEND,  ROW_TOGGLE },  // 实验性：IO18 短按回车 / 长按 Esc
  { "WiFi 配置",    0x4C7F, 0,        ROW_WIFI   },  // 进入 WiFi 配置界面
};

static bool rowConfigured(int i, const AppConfig& c) {
  switch (i) {
    case 0:  return true;          // 状态页不依赖凭据
    case 1:  return cfgOcReady(c);
    case 2:  return cfgCcReady(c);
    case 3:  return cfgDsReady(c);
    default: return true;          // 按键页 / WiFi 配置不依赖服务商凭据
  }
}

uint8_t pageMaskLoad() {
  Preferences p;
  if (!p.begin(PAGES_NVS_NS, true)) return PG_DEFAULT;
  uint8_t m = p.getUChar("mask", PG_DEFAULT);
  p.end();
  return m & PG_ALL;
}

void pageMaskSave(uint8_t mask) {
  Preferences p;
  if (!p.begin(PAGES_NVS_NS, false)) return;
  p.putUChar("mask", mask & PG_ALL);
  p.end();
}

void pageSettingsInit(PageSettings& s, uint8_t mask) {
  s.mask = mask & PG_ALL;
  s.sel  = 0;
}

static void drawSwitch(int x, int y, bool on, uint16_t accent) {
  spr.fillRoundRect(x, y, 24, 12, 6, on ? accent : 0x4208);
  spr.fillCircle(x + (on ? 18 : 6), y + 6, 4, on ? 0xFFFF : C_TEXT2);
}

void pageSettingsDraw(const PageSettings& s, const AppConfig& cfg) {
  spr.fillScreen(C_BG);

  // 页头：与用量页同一套位置（点 + 名称 + 副标题），一眼看出是同一套界面
  spr.fillCircle(UI_M + 6, 15, 5, C_ACC);
  spr.setFont(u8g2_font_wqy16_t_gb2312b);
  spr.setTextColor(C_TEXT);
  spr.setCursor(UI_M + 18, 20);
  spr.print("页面设置");

  int onCount = 0;
  for (int i = 0; i < ROW_COUNT; i++)
    if (ROWS[i].kind == ROW_TOGGLE && (s.mask & ROWS[i].bit)) onCount++;

  spr.setFont(u8g2_font_wqy12_t_gb2312b);
  spr.setTextColor(C_TEXT2);
  spr.setCursor(UI_M, 36);
  spr.print("页面开关 · WiFi 配置");
  char cnt[16];
  snprintf(cnt, sizeof(cnt), "%d/%d 页", onCount, TOGGLE_COUNT);
  spr.setCursor(UI_M + UI_CW - 36, 36);
  spr.print(cnt);

  for (int i = 0; i < ROW_COUNT; i++) {
    const int y   = ROW_Y0 + i * ROW_PITCH;
    const bool toggle = (ROWS[i].kind == ROW_TOGGLE);
    const bool on  = toggle && (s.mask & ROWS[i].bit) != 0;
    const bool sel = (s.sel == i);

    spr.fillRoundRect(UI_M, y, UI_CW, ROW_H, 6, sel ? C_SEL : C_CARD);
    if (sel) spr.drawRoundRect(UI_M, y, UI_CW, ROW_H, 6, C_TEXT2);

    spr.fillCircle(UI_M + 10, y + ROW_H / 2, 3, ROWS[i].accent);
    spr.setFont(u8g2_font_wqy12_t_gb2312b);
    spr.setTextColor(!toggle || on ? C_TEXT : C_TEXT2);
    spr.setCursor(UI_M + 18, y + 15);
    spr.print(ROWS[i].label);

    // 没凭据的页即使打开也不会出现，先把原因写在行里，省得以为是开关坏了
    if (!rowConfigured(i, cfg)) {
      spr.setTextColor(C_WARN);
      spr.setCursor(UI_M + UI_CW - 68, y + 15);
      spr.print("未配置");
    } else if (ROWS[i].bit == PG_SEND) {
      spr.setTextColor(C_WARN);
      spr.setCursor(UI_M + UI_CW - 56, y + 15);
      spr.print("实验");
    }
    if (toggle) {
      drawSwitch(UI_M + UI_CW - 30, y + 5, on, ROWS[i].accent);
    } else {
      // WiFi 行不是开关：右侧写「进入」，BOOT 打开配置界面
      spr.setTextColor(C_TEXT2);
      spr.setCursor(UI_M + UI_CW - 30, y + 15);
      spr.print("进入");
    }
  }

  const bool dsel = (s.sel == DONE_SEL);
  spr.fillRoundRect(UI_M, DONE_Y, UI_CW, DONE_H, 6, dsel ? C_ACC : C_CARD);
  spr.setFont(u8g2_font_wqy12_t_gb2312b);
  spr.setTextColor(dsel ? 0x0000 : C_TEXT);
  spr.setCursor(UI_M + 14, DONE_Y + 17);
  spr.print("完成");
  spr.setTextColor(dsel ? 0x0000 : C_TEXT2);
  spr.setCursor(UI_M + 58, DONE_Y + 17);
  spr.print("保存并返回");

  spr.setFont(u8g2_font_wqy12_t_gb2312b);
  spr.setTextColor(C_TEXT2);
  spr.setCursor(UI_M, 214);
  spr.print("PWR/IO18 选择  BOOT 确认");
}

PageSettingsEvent pageSettingsTick(PageSettings& s) {
  const HwBtn& a    = hwBtnA();     // PWR（中键）
  const HwBtn& b    = hwBtnB();     // IO18（左键）
  const HwBtn& boot = hwBtnBoot();  // BOOT（右键）

  if (a.wasPressed) s.sel = (int8_t)((s.sel + 1) % (DONE_SEL + 1));
  if (b.wasPressed) s.sel = (int8_t)((s.sel + DONE_SEL) % (DONE_SEL + 1));

  if (boot.wasPressed) {
    if (s.sel == DONE_SEL) return pgDone;
    if (ROWS[s.sel].kind == ROW_WIFI) {
      Serial.println("pages: wifi config");
      return pgWifi;
    }
    s.mask ^= ROWS[s.sel].bit;
    Serial.printf("pages: %s -> %s\n", ROWS[s.sel].label,
                  (s.mask & ROWS[s.sel].bit) ? "on" : "off");
  }
  return pgNone;
}
