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
  PG_ALL   = 0x0F,
};

uint8_t pageMaskLoad();              // 缺省 PG_ALL（全部打开）
void    pageMaskSave(uint8_t mask);  // 调用方保证值确实变了

struct PageSettings {
  uint8_t mask;      // PG_* 位组合
  int8_t  sel;       // 0..3 = 服务商，4 = 完成
};

void pageSettingsInit(PageSettings& s, uint8_t mask);
void pageSettingsDraw(const PageSettings& s, const AppConfig& cfg);
bool pageSettingsTick(PageSettings& s);   // true = 选了「完成」，请求退出
