#include "ws_client.h"
#include <Arduino.h>
#include <esp_websocket_client.h>
#include <cstring>

static esp_websocket_client_handle_t s_ws = nullptr;
static bool s_connected = false;
static WsResultCallback s_resultCb = nullptr;
static char s_resultBuf[512] = {};
static bool s_resultReady = false;

static void wsEventHandler(void* arg, esp_event_base_t base, int32_t id, void* event) {
    auto* data = static_cast<esp_websocket_event_data_t*>(event);
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        Serial.println("ws: connected");
        s_connected = true;
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        Serial.println("ws: disconnected");
        s_connected = false;
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x01 && data->data_len > 0 && data->data_ptr) {
            size_t len = data->data_len;
            if (len >= sizeof(s_resultBuf)) len = sizeof(s_resultBuf) - 1;
            memcpy(s_resultBuf, data->data_ptr, len);
            s_resultBuf[len] = '\0';
            if (strstr(s_resultBuf, "\"type\":\"result\"")) {
                s_resultReady = true;
            }
        }
        break;
    default:
        break;
    }
}

bool wsConnect(const char* host, uint16_t port) {
    if (s_ws) {
        esp_websocket_client_destroy(s_ws);
        s_ws = nullptr;
        s_connected = false;
    }

    char uri[128];
    snprintf(uri, sizeof(uri), "ws://%s:%d/audio", host, port);

    esp_websocket_client_config_t cfg = {};
    cfg.uri = uri;
    cfg.reconnect_timeout_ms = 3000;
    cfg.network_timeout_ms = 5000;
    cfg.buffer_size = 2048;

    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) {
        Serial.println("ws: init failed");
        return false;
    }

    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, wsEventHandler, nullptr);
    esp_err_t err = esp_websocket_client_start(s_ws);
    if (err != ESP_OK) {
        Serial.printf("ws: start failed: %s\n", esp_err_to_name(err));
        esp_websocket_client_destroy(s_ws);
        s_ws = nullptr;
        return false;
    }

    Serial.printf("ws: connecting to %s\n", uri);
    return true;
}

void wsDisconnect() {
    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = nullptr;
    }
    s_connected = false;
}

bool wsIsConnected() {
    return s_connected;
}

bool wsSendBin(const uint8_t* data, size_t len) {
    if (!s_ws || !s_connected) return false;
    int ret = esp_websocket_client_send_bin(s_ws, reinterpret_cast<const char*>(data), len, 500);
    return ret >= 0;
}

bool wsSendText(const char* text) {
    if (!s_ws || !s_connected) return false;
    int ret = esp_websocket_client_send_text(s_ws, text, strlen(text), 500);
    return ret >= 0;
}

void wsSetResultCallback(WsResultCallback cb) {
    s_resultCb = cb;
}

void wsPoll() {
    if (!s_ws) return;

    if (s_resultReady) {
        s_resultReady = false;
        char* textStart = strstr(s_resultBuf, "\"text\":\"");
        if (textStart) {
            textStart += 8;
            char* textEnd = strchr(textStart, '"');
            if (textEnd) *textEnd = '\0';
            if (s_resultCb) s_resultCb(textStart);
        }
    }
}