#include "ws_client.h"
#include <Arduino.h>
#include <WiFiClient.h>
#include <cstring>

static WiFiClient s_tcp;
static bool s_connected = false;
static WsResultCallback s_resultCb = nullptr;
static char s_resultBuf[512] = {};
static bool s_resultReady = false;

static void sha1(const uint8_t* data, size_t len, uint8_t* out);
static const char s_wsMagic[] = "258EAFA5-E914-47DA-95CA-CABF2A3F7AC2";

static const uint8_t b64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64Encode(const uint8_t* src, size_t len, char* dst) {
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = (uint32_t)src[i] << 16;
        if (i + 1 < len) n |= (uint32_t)src[i + 1] << 8;
        if (i + 2 < len) n |= (uint32_t)src[i + 2];
        *dst++ = b64Table[(n >> 18) & 0x3F];
        *dst++ = b64Table[(n >> 12) & 0x3F];
        *dst++ = (i + 1 < len) ? b64Table[(n >> 6) & 0x3F] : '=';
        *dst++ = (i + 2 < len) ? b64Table[n & 0x3F] : '=';
    }
    *dst = '\0';
}

static void wsSendFrame(uint8_t opcode, const uint8_t* payload, size_t len) {
    if (!s_tcp.connected()) return;
    uint8_t header[14];
    int hLen = 2;
    header[0] = 0x80 | opcode;
    if (len <= 125) {
        header[1] = (uint8_t)len;
    } else if (len <= 65535) {
        header[1] = 126;
        header[2] = (len >> 8) & 0xFF;
        header[3] = len & 0xFF;
        hLen = 4;
    } else {
        header[1] = 127;
        for (int i = 0; i < 8; i++) header[2 + i] = 0;
        header[9] = (len >> 24) & 0xFF;
        header[10] = len & 0xFF;
        hLen = 10;
    }
    s_tcp.write(header, hLen);
    if (len > 0 && payload) {
        s_tcp.write(payload, len);
    }
    s_tcp.flush();
}

static uint8_t s_recvBuf[2048];
static size_t s_recvLen = 0;

static bool wsHandshake(const char* host, uint16_t port) {
    uint8_t randKey[16];
    for (int i = 0; i < 16; i++) randKey[i] = esp_random() & 0xFF;
    char keyB64[25];
    b64Encode(randKey, 16, keyB64);

    char req[512];
    snprintf(req, sizeof(req),
        "GET /audio HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        host, port, keyB64);

    s_tcp.write((const uint8_t*)req, strlen(req));
    s_tcp.flush();

    uint32_t start = millis();
    String response;
    while (millis() - start < 3000) {
        while (s_tcp.available()) {
            char c = s_tcp.read();
            response += c;
        }
        if (response.indexOf("\r\n\r\n") >= 0) break;
        delay(10);
    }

    if (response.indexOf("101") < 0) {
        Serial.printf("ws: handshake failed: %s\n", response.c_str());
        return false;
    }

    Serial.println("ws: handshake OK");
    return true;
}

bool wsConnect(const char* host, uint16_t port) {
    wsDisconnect();
    s_connected = false;
    s_recvLen = 0;
    s_resultReady = false;

    Serial.printf("ws: connecting to %s:%d\n", host, port);
    if (!s_tcp.connect(host, port, 5000)) {
        Serial.println("ws: TCP connect failed");
        return false;
    }
    s_tcp.setNoDelay(true);

    if (!wsHandshake(host, port)) {
        s_tcp.stop();
        return false;
    }

    s_connected = true;
    Serial.println("ws: connected");
    return true;
}

void wsDisconnect() {
    if (s_connected) {
        uint8_t closeFrame[2] = {0x03, 0xE8};
        wsSendFrame(0x08, closeFrame, 2);
        delay(100);
    }
    s_tcp.stop();
    s_connected = false;
    s_recvLen = 0;
}

bool wsIsConnected() {
    return s_connected && s_tcp.connected();
}

bool wsSendBin(const uint8_t* data, size_t len) {
    if (!s_connected) return false;
    wsSendFrame(0x02, data, len);
    return true;
}

bool wsSendText(const char* text) {
    if (!s_connected) return false;
    wsSendFrame(0x01, (const uint8_t*)text, strlen(text));
    return true;
}

void wsSetResultCallback(WsResultCallback cb) {
    s_resultCb = cb;
}

static void processFrame(uint8_t opcode, const uint8_t* payload, size_t len) {
    if (opcode == 0x01 && len > 0) {
        if (len >= sizeof(s_resultBuf)) len = sizeof(s_resultBuf) - 1;
        memcpy(s_resultBuf, payload, len);
        s_resultBuf[len] = '\0';
        if (strstr(s_resultBuf, "\"type\":\"result\"")) {
            s_resultReady = true;
        }
    } else if (opcode == 0x08) {
        s_connected = false;
        s_tcp.stop();
        Serial.println("ws: server closed connection");
    }
}

void wsPoll() {
    if (!s_connected) return;

    while (s_tcp.available()) {
        size_t space = sizeof(s_recvBuf) - s_recvLen;
        int n = s_tcp.read(s_recvBuf + s_recvLen, space);
        if (n <= 0) break;
        s_recvLen += n;
    }

parse:
    if (s_recvLen < 2) return;

    uint8_t* p = s_recvBuf;
    bool fin = (p[0] & 0x80) != 0;
    uint8_t opcode = p[0] & 0x0F;
    bool masked = (p[1] & 0x80) != 0;
    uint64_t payloadLen = p[1] & 0x7F;
    size_t headerLen = 2;

    if (payloadLen == 126) {
        if (s_recvLen < 4) return;
        payloadLen = ((uint64_t)p[2] << 8) | p[3];
        headerLen = 4;
    } else if (payloadLen == 127) {
        if (s_recvLen < 10) return;
        payloadLen = 0;
        for (int i = 2; i < 10; i++) payloadLen = (payloadLen << 8) | p[i];
        headerLen = 10;
    }

    uint8_t mask[4] = {};
    if (masked) {
        if (s_recvLen < headerLen + 4) return;
        memcpy(mask, p + headerLen, 4);
        headerLen += 4;
    }

    if (s_recvLen < headerLen + payloadLen) return;

    uint8_t* payloadPtr = p + headerLen;
    if (masked) {
        for (uint64_t i = 0; i < payloadLen; i++) {
            payloadPtr[i] ^= mask[i & 3];
        }
    }

    processFrame(opcode, payloadPtr, (size_t)payloadLen);

    size_t consumed = headerLen + (size_t)payloadLen;
    s_recvLen -= consumed;
    if (s_recvLen > 0) {
        memmove(s_recvBuf, s_recvBuf + consumed, s_recvLen);
    }
    goto parse;
}