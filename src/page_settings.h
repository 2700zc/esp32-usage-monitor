#pragma once
#include <stdint.h>

struct AppConfig;

// 页面开关：位序与 main.cpp 的 Prov 一致，整组开关持久化在 NVS（"pages"/"mask"）。
// 关掉的页面既不显示，也不进入 60 秒抓取轮次——过期凭据不会再被反复请求。
enum : uint8_t {
  PG_AGENT = 1u << 0,
  PG_OC    = 1u << 1,
  PG_CC    = 1u << 2,
  PG_DS    = 1u << 3,
  PG_SEND  = 1u << 4,   // 按键页（实验性），默认关闭
  PG_ALL     = 0x1F,
  // 缺省值：老设备升级后行为不变，实验性页面要手动打开才出现
  PG_DEFAULT = PG_AGENT | PG_OC | PG_CC | PG_DS,
};

uint8_t pageMaskLoad();              // 缺省 PG_DEFAULT（按键页关）
void    pageMaskSave(uint8_t mask);  // 调用方保证值确实变了

struct PageSettings {
  uint8_t mask;      // PG_* 位组合
  int8_t  sel;       // 0..5 = 设置项，6 = 完成
};

// 设置页的动作结果。WiFi 配置也收在这一页里，BOOT 在 WiFi 行上是「进入」
// 而不是切换，所以不能再用 bool 表示（true 只代表「完成」）。
enum PageSettingsEvent : uint8_t {
  pgNone = 0,
  pgDone,            // 选了「完成」，调用方保存 mask 并退出
  pgWifi,            // 选了「WiFi 配置」，调用方打开 WiFi 配置界面
};

void pageSettingsInit(PageSettings& s, uint8_t mask);
void pageSettingsDraw(const PageSettings& s, const AppConfig& cfg);
PageSettingsEvent pageSettingsTick(PageSettings& s);
