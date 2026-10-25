#pragma once
#include <Arduino.h>
#include <stdio.h>
#include <string.h>

// 通用「用量窗口」——滚动 / 每周 / 每月这类有配额上限的窗口统一用它。
// OpenCode 与 CommandCode 的响应结构完全不同，但归一化后都是这几个字段。
struct UsageSegment {
  bool valid;         // 是否解析出数值（false 时界面显示 --）
  float percent;      // 已用百分比，如 8.1
  uint32_t resetSec;  // 距重置秒数，0 = 未知
  char status[16];    // "ok" / "rate-limited" / "unlimited" / "unavailable"
};

// 倒计时文案，与 TokenHub 的 format_reset_time 一致：
// 有天则「X天Y小时Z分后重置」，否则「X小时Y分后重置」；未知写 "--"
inline void usageFormatReset(char* buf, size_t sz, uint32_t resetSec) {
  if (resetSec == 0) { strlcpy(buf, "--", sz); return; }
  uint32_t days  = resetSec / 86400;
  uint32_t hours = (resetSec % 86400) / 3600;
  uint32_t mins  = (resetSec % 3600) / 60;
  if (days > 0) snprintf(buf, sz, "%lu天%lu小时%lu分后重置",
                         (unsigned long)days, (unsigned long)hours, (unsigned long)mins);
  else          snprintf(buf, sz, "%lu小时%lu分后重置",
                         (unsigned long)hours, (unsigned long)mins);
}

// 紧凑版，去掉「后重置」三个字：卡片标签行右侧空间有限，
// 全称「16天6小时40分后重置」在 wqy12 下宽 114px，会压到左边的标签。
inline void usageFormatResetShort(char* buf, size_t sz, uint32_t resetSec) {
  if (resetSec == 0) { strlcpy(buf, "--", sz); return; }
  uint32_t days  = resetSec / 86400;
  uint32_t hours = (resetSec % 86400) / 3600;
  uint32_t mins  = (resetSec % 3600) / 60;
  if (days > 0) snprintf(buf, sz, "%lu天%lu小时%lu分",
                         (unsigned long)days, (unsigned long)hours, (unsigned long)mins);
  else          snprintf(buf, sz, "%lu小时%lu分",
                         (unsigned long)hours, (unsigned long)mins);
}
