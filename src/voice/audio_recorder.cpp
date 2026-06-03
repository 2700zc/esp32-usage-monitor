#include "audio_recorder.h"
#include "i2s_audio.h"
#include "ws_client.h"
#include <Arduino.h>
#include <cstring>
#include <cstdio>

static const size_t PCM_CHUNK = 640;

static RecorderContext* s_ctx = nullptr;

static void onWsMessage(const char* msg) {
    if (!s_ctx) return;
    if (s_ctx->state == RecorderState::Uploading) {
        if (strstr(msg, "\"saved\"")) {
            const char* p = strstr(msg, "\"path\":\"");
            if (p) {
                p += 8;
                int i = 0;
                while (*p && *p != '"' && i < 63) {
                    s_ctx->savePath[i++] = *p++;
                }
                s_ctx->savePath[i] = '\0';
            }
            s_ctx->state = RecorderState::Done;
            s_ctx->stateSince = millis();
            Serial.printf("recorder: saved as %s\n", s_ctx->savePath);
        } else if (strstr(msg, "\"error\"")) {
            s_ctx->state = RecorderState::Failed;
            s_ctx->stateSince = millis();
            Serial.println("recorder: pc error");
        }
    }
}

void recorderInit(RecorderContext& ctx) {
    s_ctx = &ctx;
    ctx.state = RecorderState::Idle;
    ctx.stateSince = 0;
    ctx.savePath[0] = '\0';
    wsSetCallback(onWsMessage);
}

bool recorderStart(RecorderContext& ctx, const char* pcHost, uint16_t pcPort) {
    if (ctx.state != RecorderState::Idle) return false;

    wsSetCallback(onWsMessage);

    if (!wsConnect(pcHost, pcPort)) {
        Serial.println("recorder: ws connect failed");
        return false;
    }

    uint32_t t0 = millis();
    while (!wsIsConnected() && millis() - t0 < 3000) {
        delay(10);
    }
    if (!wsIsConnected()) {
        Serial.println("recorder: ws connect timeout");
        wsDisconnect();
        return false;
    }

    if (!audioRxStart()) {
        Serial.println("recorder: audio rx start failed");
        wsDisconnect();
        return false;
    }

    wsSendText("{\"type\":\"start\"}");

    ctx.state = RecorderState::Recording;
    ctx.stateSince = millis();
    ctx.savePath[0] = '\0';
    Serial.println("recorder: started");
    return true;
}

void recorderTick(RecorderContext& ctx) {
    if (ctx.state != RecorderState::Recording) return;

    uint8_t buf[PCM_CHUNK];
    size_t got = audioRxRead(buf, sizeof(buf));
    if (got > 0 && wsIsConnected()) {
        wsSendBin(buf, got);
    }
}

void recorderStop(RecorderContext& ctx, const char* pcHost, uint16_t pcPort) {
    if (ctx.state != RecorderState::Recording) return;

    audioRxStop();

    ctx.state = RecorderState::Uploading;
    ctx.stateSince = millis();

    wsSendText("{\"type\":\"stop\"}");
    Serial.println("recorder: stop sent, waiting for PC response");
}

void recorderReset(RecorderContext& ctx) {
    wsDisconnect();
    ctx.state = RecorderState::Idle;
    ctx.stateSince = 0;
    ctx.savePath[0] = '\0';
}
