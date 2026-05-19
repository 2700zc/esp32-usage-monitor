#include "wifi_setup.h"
#include "hw/hw.h"
#include "hw/net.h"
#include "hw/audio.h"
#include <string.h>

#define spr (*hwCanvas())

static const uint16_t COL_BG      = 0x0000;
static const uint16_t COL_TEXT    = 0xFFFF;
static const uint16_t COL_TEXTDIM = 0x8410;
static const uint16_t COL_BODY    = 0xC2A6;
static const uint16_t GREEN       = 0x07E0;
static const uint16_t HOT         = 0xFA20;

static const char* WIFI_CHARS[] = {
  "abcdefghijklmnopqrstuvwxyz",
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
  "0123456789",
  " !@#$%^&*()_+-=[]{}|;:,.<>?/~",
};
static const char* WIFI_GROUP_NAMES[] = { "abc", "ABC", "123", "!@" };

static NetScanResult wifiNets[20];

void wifiSetupInit(WifiState& state) {
  memset(&state, 0, sizeof(state));
  state.open = true;
  state.step = 0;
  state.scanStart = millis();
}

void wifiSetupDraw(WifiState& state) {
  int W = HW_W;
  int H = HW_H;

  spr.fillRect(0, 56, W, H - 56, COL_BG);
  spr.setTextSize(1);
  int y = 60;

  auto hdr = [&](const char* s) {
    spr.setTextColor(COL_TEXT, COL_BG); spr.setCursor(SAFE_L, y); spr.print(s); y += 11;
  };
  auto ln = [&](uint16_t c, const char* s) {
    spr.setTextColor(c, COL_BG); spr.setCursor(SAFE_L, y); spr.print(s); y += 9;
  };

  if (state.step == 0) {
    hdr("Scanning...");
    uint32_t e = (millis() - state.scanStart) / 1000;
    if (e >= 2 && state.netsCount == 0) {
      NetScanResult buf[20];
      state.netsCount = netScan(buf, 20);
      memcpy(wifiNets, buf, state.netsCount * sizeof(NetScanResult));
      char saved[33] = "";
      char dummyPass[64];
      netLoadCred(saved, 33, dummyPass, 64);
      int si = -1;
      for (int i = 0; i < state.netsCount; i++) {
        if (strcmp(wifiNets[i].ssid, saved) == 0) { si = i; break; }
      }
      state.step = 1;
      state.sel = (si >= 0) ? si : 0;
    }
    ln(COL_TEXTDIM, "wait...");

  } else if (state.step == 1) {
    hdr("Select WiFi");
    for (int i = 0; i < state.netsCount && y < H - 14; i++) {
      NetScanResult* n = &wifiNets[i];
      spr.setTextColor(i == state.sel ? COL_TEXT : COL_TEXTDIM, COL_BG);
      spr.setCursor(SAFE_L, y);
      spr.printf("%c ", i == state.sel ? '>' : ' ');
      int maxW = (W - SAFE_L - 36) / 6;
      int sl = strlen(n->ssid);
      if (sl > maxW) { char b[33]; memcpy(b, n->ssid, maxW); b[maxW] = 0; spr.print(b); }
      else spr.print(n->ssid);
      spr.setCursor(W - 30, y);
      spr.printf("%d", n->rssi);
      y += 10;
    }
    ln(COL_TEXTDIM, "A:scroll  B:select");

  } else if (state.step == 2) {
    hdr("Password");
    spr.setTextColor(COL_TEXTDIM, COL_BG);
    spr.setCursor(SAFE_L, y);
    char ssidShow[22];
    strncpy(ssidShow, wifiNets[state.sel].ssid, 21); ssidShow[21] = 0;
    spr.print(ssidShow); y += 11;
    spr.setTextColor(COL_TEXT, COL_BG);
    spr.setCursor(SAFE_L, y);
    int pl = strlen(state.pass);
    for (int i = 0; i < pl; i++) spr.print('*');
    spr.print(pl < 63 ? '_' : ' '); y += 10;
    const char* g = WIFI_CHARS[state.charGroup];
    int gl = strlen(g);
    int st = state.charIdx - 7;
    if (st < 0) st = 0;
    if (st + 15 > gl) st = gl - 15;
    if (st < 0) st = 0;
    spr.setTextColor(COL_TEXTDIM, COL_BG);
    spr.setCursor(SAFE_L, y);
    for (int i = st; i < gl && i < st + 15; i++) {
      spr.setTextColor(i == state.charIdx ? COL_TEXT : COL_TEXTDIM, COL_BG);
      spr.print(g[i]);
    }
    y += 10;
    spr.setTextColor(COL_BODY, COL_BG);
    spr.setCursor(SAFE_L, y); spr.print(WIFI_GROUP_NAMES[state.charGroup]);
    spr.setCursor(W / 2 - 12, y); spr.print("[DEL]");
    spr.setCursor(W / 2 + 18, y); spr.print("[OK]");
    y += 10;
    ln(COL_TEXTDIM, "A:char  Ahold:group");
    ln(COL_TEXTDIM, "B:add  Bhold:connect");

  } else if (state.step == 3) {
    if (!state.connDone) {
      state.connDone = true;
      hdr("Connecting...");
      ln(COL_TEXTDIM, wifiNets[state.sel].ssid);
      hwDisplayPush();
      state.connOk = netConnect(wifiNets[state.sel].ssid, state.pass);
    }
    if (state.connOk) {
      hdr("Connected!");
      ln(GREEN, "IP: ");
      spr.setTextColor(GREEN, COL_BG);
      spr.print(netIP().toString().c_str());
      y += 9;
      ln(COL_TEXTDIM, "Any key to exit");
    } else {
      hdr("Failed");
      ln(HOT, "Could not connect");
      ln(COL_TEXTDIM, "Any key to exit");
    }
  }
}

void wifiSetupHandleButtons(WifiState& state) {
  bool btnALong = false;
  bool swallowB = false;

  if (hwBtnA().pressedFor(600)) {
    btnALong = true;
    if (state.step == 2) {
      state.charGroup = (state.charGroup + 1) % 4;
      state.charIdx = 0;
      hwBeep(800, 40);
    } else {
      state.open = false;
      hwBeep(800, 40);
    }
  }
  if (hwBtnA().wasReleased && !btnALong) {
    if (state.step == 1) {
      state.sel = (state.sel + 1) % state.netsCount;
      hwBeep(1800, 20);
    } else if (state.step == 2) {
      const char* g = WIFI_CHARS[state.charGroup];
      state.charIdx = (state.charIdx + 1) % strlen(g);
      hwBeep(1800, 20);
    } else if (state.step == 3 && state.connDone) {
      state.open = false;
    }
  }
  if (hwBtnB().pressedFor(800)) {
    swallowB = true;
    if (state.step == 2) {
      state.charGroup = 0;
      state.charIdx = 0;
      state.step = 3;
      state.connDone = false;
      hwBeep(1200, 60);
    } else if (state.step == 3 && state.connDone) {
      state.open = false;
    }
  }
  if (hwBtnB().wasPressed && !swallowB) {
    if (state.step == 1) {
      state.step = 2;
      state.pass[0] = 0;
      state.charGroup = 0;
      state.charIdx = 0;
      if (wifiNets[state.sel].encryption == 0) {
        state.pass[0] = 0;
        state.step = 3;
        state.connDone = false;
      }
      hwBeep(2400, 30);
    } else if (state.step == 2) {
      int pl = strlen(state.pass);
      if (pl < 63) {
        state.pass[pl] = WIFI_CHARS[state.charGroup][state.charIdx];
        state.pass[pl + 1] = 0;
      }
      hwBeep(1800, 20);
    } else if (state.step == 3 && state.connDone) {
      state.open = false;
    }
  }
}
