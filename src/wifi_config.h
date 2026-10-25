#pragma once
#include "hw/net.h"

enum class WfState {
    Scanning,
    ScanList,
    Password,
    Connecting,
    Connected,
    Failed
};

struct WfConfig {
    WfState state = WfState::Scanning;
    NetScanResult networks[8];
    int8_t networkCount = 0;
    int8_t listSel = 0;
    int8_t scrollOff = 0;
    char password[65] = {};
    int8_t passwordLen = 0;
    int8_t kbMode = 0;      // 0=abc 1=ABC 2=123
    int8_t curRow = 0;
    int8_t curCol = 0;
    char ssid[33] = {};
    uint32_t connectStart = 0;
};

void wfInit(WfConfig& cfg);
void wfDraw(WfConfig& cfg);
bool wfTick(WfConfig& cfg);