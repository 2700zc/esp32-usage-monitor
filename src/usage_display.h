#pragma once
#include "api_client.h"

void usageDisplayDraw(const UsageData& data, const char* ip = nullptr);
void usageDisplayDrawTime(const char* ip, bool timeValid = false);
void usageDisplayDrawThinking(uint32_t elapsedMs, int step, const char* msg);
void usageDisplayDrawDone(int steps, uint32_t elapsedMs);
void usageDisplayDrawFailed(uint32_t elapsedMs);
void usageDisplayDrawEaster555(uint32_t elapsedMs);
