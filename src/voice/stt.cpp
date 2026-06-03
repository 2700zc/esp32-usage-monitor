#include "stt.h"
#include "i2s_audio.h"
#include <Arduino.h>
#include <WiFi.h>
#include <cstring>

static void writeWavHeader(uint8_t* hdr, size_t pcmLen) {
    uint32_t fileSize = 36 + pcmLen;
    uint32_t byteRate = SttContext::SAMPLE_RATE * SttContext::BYTES_PER_SAMPLE;
    uint16_t blockAlign = SttContext::BYTES_PER_SAMPLE;
    uint16_t bitsPerSample = 16;
    uint16_t numChannels = 1;

    memcpy(hdr + 0,  "RIFF", 4);
    memcpy(hdr + 4,  &fileSize, 4);
    memcpy(hdr + 8,  "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    uint32_t fmtSize = 16;
    memcpy(hdr + 16, &fmtSize, 4);
    uint16_t audioFormat = 1;
    memcpy(hdr + 20, &audioFormat, 2);
    memcpy(hdr + 22, &numChannels, 2);
    uint32_t sampleRate = SttContext::SAMPLE_RATE;
    memcpy(hdr + 24, &sampleRate, 4);
    memcpy(hdr + 28, &byteRate, 4);
    memcpy(hdr + 32, &blockAlign, 2);
    memcpy(hdr + 34, &bitsPerSample, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &pcmLen, 4);
}

void sttInit(SttContext& ctx) {
    ctx.state = SttState::Idle;
    ctx.pcmBuf = nullptr;
    ctx.pcmLen = 0;
    ctx.pcmCap = 0;
    ctx.resultText[0] = '\0';
}

SttState sttStartRecording(SttContext& ctx) {
    if (ctx.state != SttState::Idle) return ctx.state;

    ctx.pcmBuf = (uint8_t*)heap_caps_malloc(SttContext::MAX_PCM_BYTES, MALLOC_CAP_SPIRAM);
    if (!ctx.pcmBuf) {
        ctx.pcmBuf = (uint8_t*)malloc(SttContext::MAX_PCM_BYTES);
    }
    if (!ctx.pcmBuf) {
        Serial.println("stt: failed to allocate PCM buffer");
        ctx.state = SttState::Idle;
        return ctx.state;
    }
    ctx.pcmLen = 0;
    ctx.pcmCap = SttContext::MAX_PCM_BYTES;
    ctx.resultText[0] = '\0';

    if (!audioRxStart()) {
        free(ctx.pcmBuf);
        ctx.pcmBuf = nullptr;
        ctx.state = SttState::Idle;
        return ctx.state;
    }

    ctx.state = SttState::Recording;
    ctx.stateSince = millis();
    Serial.println("stt: recording started");
    return ctx.state;
}

SttState sttTick(SttContext& ctx) {
    if (ctx.state != SttState::Recording) return ctx.state;

    size_t remain = ctx.pcmCap - ctx.pcmLen;
    if (remain > 0) {
        size_t toRead = (remain > 1024) ? 1024 : remain;
        size_t got = audioRxRead(ctx.pcmBuf + ctx.pcmLen, toRead);
        ctx.pcmLen += got;
    }

    uint32_t elapsed = millis() - ctx.stateSince;
    if (elapsed >= SttContext::MAX_RECORD_MS) {
        Serial.println("stt: max duration reached, auto-stopping");
        return SttState::Recording;
    }

    return ctx.state;
}

static bool httpPostWav(const uint8_t* wavData, size_t wavLen,
                        const char* pcHost, uint16_t pcPort,
                        char* resultText, size_t resultTextCap) {
    WiFiClient client;
    client.setTimeout(30000);

    Serial.printf("stt: connecting to %s:%u ...\n", pcHost, pcPort);
    if (!client.connect(pcHost, pcPort, 15000)) {
        Serial.println("stt: connect failed");
        return false;
    }

    Serial.printf("stt: POST /stt (%u bytes) ...\n", (unsigned)wavLen);
    client.printf("POST /stt HTTP/1.1\r\n");
    client.printf("Host: %s:%u\r\n", pcHost, pcPort);
    client.printf("Content-Type: audio/wav\r\n");
    client.printf("Content-Length: %u\r\n", (unsigned)wavLen);
    client.printf("Connection: close\r\n");
    client.printf("\r\n");

    const size_t chunkSize = 4096;
    size_t sent = 0;
    uint32_t sendStart = millis();
    while (sent < wavLen) {
        size_t n = (wavLen - sent > chunkSize) ? chunkSize : (wavLen - sent);
        size_t wrote = client.write(wavData + sent, n);
        if (wrote == 0) {
            Serial.println("stt: write failed");
            client.stop();
            return false;
        }
        sent += wrote;
        if (millis() - sendStart > 30000) {
            Serial.println("stt: send timeout");
            client.stop();
            return false;
        }
    }

    Serial.println("stt: waiting for response ...");
    String response;
    uint32_t respStart = millis();
    while (millis() - respStart < 60000) {
        while (client.available()) {
            char c = client.read();
            response += c;
            if (response.length() > 4096) {
                Serial.println("stt: response too large");
                client.stop();
                return false;
            }
        }
        if (!client.connected()) break;
        delay(10);
    }
    client.stop();

    int bodyIdx = response.indexOf("\r\n\r\n");
    if (bodyIdx < 0) {
        Serial.println("stt: no body in response");
        return false;
    }
    String body = response.substring(bodyIdx + 4);
    body.trim();
    if (body.length() > 0) {
        strncpy(resultText, body.c_str(), resultTextCap - 1);
        resultText[resultTextCap - 1] = '\0';
        return true;
    }
    Serial.println("stt: empty response body");
    return false;
}

SttState sttStopRecording(SttContext& ctx, const char* pcHost, uint16_t pcPort) {
    if (ctx.state != SttState::Recording) return ctx.state;

    audioRxStop();

    if (ctx.pcmLen < 1600) {
        Serial.printf("stt: too short (%u bytes), discarding\n", (unsigned)ctx.pcmLen);
        free(ctx.pcmBuf);
        ctx.pcmBuf = nullptr;
        ctx.state = SttState::Failed;
        ctx.stateSince = millis();
        return ctx.state;
    }

    size_t wavLen = 44 + ctx.pcmLen;
    uint8_t* wavBuf = (uint8_t*)heap_caps_malloc(wavLen, MALLOC_CAP_SPIRAM);
    if (!wavBuf) wavBuf = (uint8_t*)malloc(wavLen);
    if (!wavBuf) {
        free(ctx.pcmBuf);
        ctx.pcmBuf = nullptr;
        ctx.state = SttState::Failed;
        ctx.stateSince = millis();
        return ctx.state;
    }

    writeWavHeader(wavBuf, ctx.pcmLen);
    memcpy(wavBuf + 44, ctx.pcmBuf, ctx.pcmLen);
    free(ctx.pcmBuf);
    ctx.pcmBuf = nullptr;

    ctx.state = SttState::Uploading;
    ctx.stateSince = millis();

    bool ok = httpPostWav(wavBuf, wavLen, pcHost, pcPort, ctx.resultText, sizeof(ctx.resultText));
    free(wavBuf);

    if (ok && ctx.resultText[0] != '\0') {
        ctx.state = SttState::Success;
        Serial.printf("stt: result: %s\n", ctx.resultText);
    } else {
        ctx.state = SttState::Failed;
        Serial.println("stt: upload/recognition failed");
    }
    ctx.stateSince = millis();
    return ctx.state;
}

void sttReset(SttContext& ctx) {
    if (ctx.pcmBuf) {
        free(ctx.pcmBuf);
        ctx.pcmBuf = nullptr;
    }
    ctx.state = SttState::Idle;
    ctx.stateSince = 0;
    ctx.pcmLen = 0;
}