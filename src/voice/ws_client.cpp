#include "ws_client.h"
#include <Arduino.h>
#include <esp_transport.h>
#include <esp_transport_tcp.h>
#include <esp_transport_ws.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static esp_transport_handle_t s_ws = nullptr;
static WsCallback s_cb = nullptr;
static TaskHandle_t s_readTask = nullptr;
static volatile bool s_running = false;

static void wsReadTask(void* arg) {
    char buf[2048];
    while (s_running && s_ws) {
        int ret = esp_transport_read(s_ws, buf, sizeof(buf) - 1, 100);
        if (ret > 0 && s_cb) {
            ws_transport_opcodes_t opcode = esp_transport_ws_get_read_opcode(s_ws);
            if (opcode == WS_TRANSPORT_OPCODES_TEXT) {
                buf[ret] = '\0';
                s_cb(buf);
            }
        } else if (ret == ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN) {
            Serial.println("ws: connection closed by peer");
            break;
        } else if (ret < 0 && ret != ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT) {
            if (s_running) {
                Serial.printf("ws: read error %d\n", ret);
            }
            break;
        }
    }
    s_running = false;
    vTaskDelete(nullptr);
}

bool wsConnect(const char* host, uint16_t port) {
    if (s_ws) wsDisconnect();

    esp_transport_handle_t tcp = esp_transport_tcp_init();
    if (!tcp) {
        Serial.println("ws: tcp init failed");
        return false;
    }

    esp_transport_keep_alive_t ka = {};
    ka.keep_alive_enable = true;
    ka.keep_alive_idle = 30;
    ka.keep_alive_interval = 10;
    ka.keep_alive_count = 3;
    esp_transport_tcp_set_keep_alive(tcp, &ka);

    s_ws = esp_transport_ws_init(tcp);
    if (!s_ws) {
        Serial.println("ws: ws init failed");
        esp_transport_destroy(tcp);
        return false;
    }

    esp_transport_ws_set_path(s_ws, "/");

    int ret = esp_transport_connect(s_ws, host, port, 10000);
    if (ret != 0) {
        Serial.printf("ws: connect failed: %d\n", ret);
        esp_transport_destroy(s_ws);
        s_ws = nullptr;
        return false;
    }

    Serial.printf("ws: connected to %s:%u\n", host, port);

    s_running = true;
    BaseType_t taskCreated = xTaskCreate(wsReadTask, "ws_recv", 4096, nullptr, 5, &s_readTask);
    if (taskCreated != pdPASS) {
        Serial.println("ws: failed to create read task");
        s_running = false;
        esp_transport_destroy(s_ws);
        s_ws = nullptr;
        return false;
    }

    return true;
}

void wsSendBin(const uint8_t* data, size_t len) {
    if (!s_ws) return;
    esp_transport_write(s_ws, (const char*)data, (int)len, portMAX_DELAY);
}

void wsSendText(const char* json) {
    if (!s_ws) return;
    esp_transport_ws_send_raw(s_ws, WS_TRANSPORT_OPCODES_TEXT, json, (int)strlen(json), portMAX_DELAY);
}

bool wsIsConnected() {
    return s_ws != nullptr && s_running;
}

void wsDisconnect() {
    s_running = false;
    if (s_readTask) {
        vTaskDelete(s_readTask);
        s_readTask = nullptr;
    }
    if (s_ws) {
        esp_transport_close(s_ws);
        esp_transport_destroy(s_ws);
        s_ws = nullptr;
    }
    s_cb = nullptr;
}

void wsSetCallback(WsCallback cb) {
    s_cb = cb;
}
