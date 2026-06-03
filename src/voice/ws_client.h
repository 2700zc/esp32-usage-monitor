#pragma once
#include <stdint.h>
#include <stddef.h>
#include <functional>

using WsCallback = std::function<void(const char* msg)>;

bool wsConnect(const char* host, uint16_t port);
void wsSendBin(const uint8_t* data, size_t len);
void wsSendText(const char* json);
bool wsIsConnected();
void wsDisconnect();
void wsSetCallback(WsCallback cb);
