#pragma once
#include <stdint.h>
#include "hw/net.h"

struct WifiState {
  bool open;
  int step;         // 0=scanning, 1=list, 2=password, 3=connecting
  int sel;
  char pass[64];
  int charGroup;    // 0=lower, 1=upper, 2=digits, 3=symbols
  int charIdx;
  int netsCount;
  bool connected;
  bool connDone;
  bool connOk;
  uint32_t scanStart;
};

void wifiSetupInit(WifiState& state);
void wifiSetupDraw(WifiState& state);
void wifiSetupHandleButtons(WifiState& state);
