#define spr (*hwCanvas())

#include "config.h"
#include "wifi_setup.h"
#include "api_client.h"
#include "usage_display.h"
#include "hw/hw.h"
#include "hw/net.h"

enum AppState {
  STATE_WIFI_SETUP,
  STATE_DESKTOP,
};

static AppState g_state = STATE_WIFI_SETUP;
static WifiState s_wifi;
static UsageData s_usage;
static AppConfig s_cfg;
static uint32_t s_lastFetch = 0;
static bool s_firstDesktop = true;

void setup() {
  hwInit();
  loadConfig(s_cfg);
  wifiSetupInit(s_wifi);
}

void loop() {
  hwInputUpdate();

  switch (g_state) {

    case STATE_WIFI_SETUP: {
      wifiSetupDraw(s_wifi);
      wifiSetupHandleButtons(s_wifi);
      if (WiFi.status() == WL_CONNECTED && hwBtnA().pressedFor(600)) {
        g_state = STATE_DESKTOP;
        s_firstDesktop = true;
        hwBeep(1200, 60);
      }
      break;
    }

    case STATE_DESKTOP: {
      if (s_firstDesktop) {
        s_firstDesktop = false;
        s_lastFetch = 0;
      }

      uint32_t now = millis();
      if (now - s_lastFetch >= 60000 || s_lastFetch == 0) {
        s_lastFetch = now;
        apiFetchUsage(s_usage, s_cfg.server_id, s_cfg.cookie, s_cfg.workspace_id);
      }

      if (hwBtnA().wasPressed) {
        s_lastFetch = 0;
      }

      usageDisplayDraw(s_usage, WiFi.localIP().toString().c_str());
      break;
    }
  }

  hwDisplayPush();
  delay(16);
}
