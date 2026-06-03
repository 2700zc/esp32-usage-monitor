#pragma once
#include <stdint.h>
#include <stddef.h>

enum class SttState {
    Idle,
    Recording,
    Uploading,
    Success,
    Failed
};

struct SttContext {
    SttState state = SttState::Idle;
    uint32_t stateSince = 0;
    uint8_t* pcmBuf = nullptr;
    size_t pcmLen = 0;
    size_t pcmCap = 0;
    char resultText[128] = "";
    static constexpr size_t MAX_RECORD_MS = 10000;
    static constexpr size_t SAMPLE_RATE = 16000;
    static constexpr size_t BYTES_PER_SAMPLE = 2;
    static constexpr size_t MAX_PCM_BYTES = SAMPLE_RATE * BYTES_PER_SAMPLE * MAX_RECORD_MS / 1000;
};

void sttInit(SttContext& ctx);
SttState sttStartRecording(SttContext& ctx);
SttState sttTick(SttContext& ctx);
SttState sttStopRecording(SttContext& ctx, const char* pcHost, uint16_t pcPort);
void sttReset(SttContext& ctx);