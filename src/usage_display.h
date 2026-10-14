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

// 一个服务商都没配置时的空状态
void usageDisplayDrawEmpty(const char* ip);
