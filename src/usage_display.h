#pragma once
#include "api_client.h"

void usageDisplayDraw(const UsageData& data, const char* ip = nullptr);
void usageDisplayDrawTime(const char* ip, bool timeValid = false);
void usageDisplayDrawThinking(uint32_t animStartMs);
