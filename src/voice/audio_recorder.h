#pragma once
#include <stdint.h>

enum class RecorderState {
    Idle,
    Recording,
    Uploading,
    Done,
    Failed
};

struct RecorderContext {
    RecorderState state = RecorderState::Idle;
    uint32_t stateSince = 0;
    char savePath[64] = "";
};

void recorderInit(RecorderContext& ctx);
bool recorderStart(RecorderContext& ctx, const char* pcHost, uint16_t pcPort);
void recorderTick(RecorderContext& ctx);
void recorderStop(RecorderContext& ctx, const char* pcHost, uint16_t pcPort);
void recorderReset(RecorderContext& ctx);
