#pragma once
#include "agent_client.h"
#include "deepseek_client.h"
#include "opencode_client.h"
#include "commandcode_client.h"

// 每个页面自行绘制整块画布。page/pageCount 只用于底部页码圆点（单页时不画）。
//
// 刷新失败时页面**不会**跳走：只要曾经成功获取过，就继续显示上一次的数据，
// 页头副标题改成琥珀色的「失败原因 · 数据年龄」。从没成功过才整页显示错误。
//
// netStatus 非空时用作页头状态文案（WiFi 断开时传「网络断开」），优先级高于
// 数据自带的 lastError。
void usageDisplayDrawAgent(const AgentStatus& data, const char* ip, int page, int pageCount,
                           const char* netStatus = nullptr);
void usageDisplayDrawDs(const DeepSeekUsage& data, const char* ip, int page, int pageCount,
                        const char* netStatus = nullptr);
void usageDisplayDrawOc(const OpenCodeUsage& data, const char* ip, int page, int pageCount,
                        const char* netStatus = nullptr);
void usageDisplayDrawCc(const CommandCodeUsage& data, const char* ip, int page, int pageCount,
                        const char* netStatus = nullptr);

// 按键页（实验性）：显示物理键映射与最近一次动作结果。动作由 IO18 触发
// （短按回车、长按 Esc），触摸页只做说明，避免触摸坐标在旋转后的命中问题。
struct SendUiState {
  bool     busy;        // 请求在途
  int8_t   busyAction;  // 0 = 回车，1 = Esc
  int8_t   result;      // AgActCode；-1 = 还没按过
  int8_t   lastAction;  // 最近一次动作：0 = 回车，1 = Esc
  uint32_t atMs;        // 结果产生时的 millis（用于「N 分钟前」）
};

// bridge 用来显示桥接连通性；按键页打开时后台仍在发现/轮询桥接。
void usageDisplayDrawSend(const AgentStatus& bridge, const SendUiState& ui,
                          const char* ip, int page, int pageCount,
                          const char* netStatus = nullptr);

// 一个页面都没有时的空状态。pagesOff = 页面开关全关（与"没配凭据"区分开，
// 前者要提示怎么把开关打开）。
void usageDisplayDrawEmpty(const char* ip, bool pagesOff);
