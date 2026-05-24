#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

bool wsConnect(const char* host, uint16_t port);
void wsDisconnect();
bool wsIsConnected();
bool wsSendBin(const uint8_t* data, size_t len);
bool wsSendText(const char* text);

typedef void (*WsResultCallback)(const char* text);
void wsSetResultCallback(WsResultCallback cb);
void wsPoll();