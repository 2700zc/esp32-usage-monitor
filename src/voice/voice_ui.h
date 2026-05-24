#pragma once
#include <cstdint>
void voiceUiDrawIdle(const char* ip, bool wsConnected);
void voiceUiDrawListening(uint32_t elapsedMs, bool wsConnected);
void voiceUiDrawProcessing();
void voiceUiDrawResult(const char* text);
void voiceUiDrawNoConfig();