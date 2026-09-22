#define spr (*hwCanvas())

#include "config.h"
#include "deepseek_client.h"
#include "usage_display.h"
#include "hw/hw.h"
#include "hw/net.h"
#include "wifi_config.h"
#include <WiFi.h>
#include <time.h>
#include <U8g2lib.h>

static const uint16_t MAIN_BG     = 0x0000;
static const uint16_t MAIN_YELLOW = 0xFFE0;

static const uint32_t FETCH_INTERVAL_MS = 30000;  // 每 30 秒刷新
static const uint32_t CONNECT_TIMEOUT_MS = 10000; // 自动连接 10 秒超时

static DeepSeekUsage s_usage;
static AppConfig s_cfg;
static uint32_t s_lastFetch = 0;
static uint32_t s_wifiConnStart = 0;  // 自动连接开始时刻；0 = 未在自动连接
static bool s_wifiConnected = false;
static WfConfig s_wfCfg;
static bool s_inWifiConfig = false;

static void startNtp() {
  configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
}

void setup() {
  hwInit();

  spr.fillScreen(MAIN_BG);
  spr.setFont(u8g2_font_wqy16_t_gb2312b);
  spr.setTextColor(MAIN_YELLOW);
  spr.setCursor(SAFE_L, SAFE_T + 60);
  spr.print("Booting...");
  hwDisplayPush();

  loadConfig(s_cfg);

  // 优先自动连接上次保存的 WiFi；没有凭据则直接进配置界面
  char savedSSID[33], savedPass[65];
  if (netLoadCred(savedSSID, 33, savedPass, 65)) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(savedSSID, savedPass);
    s_wifiConnStart = millis();
  } else {
    s_inWifiConfig = true;
    wfInit(s_wfCfg);
  }
}

void loop() {
  hwInputUpdate();

  if (!s_wifiConnected) {
    if (WiFi.status() == WL_CONNECTED) {
      s_wifiConnected = true;
      s_wifiConnStart = 0;
      startNtp();
      s_lastFetch = 0;
    } else if (s_wifiConnStart != 0 && !s_inWifiConfig &&
               millis() - s_wifiConnStart >= CONNECT_TIMEOUT_MS) {
      // 自动连接超时：回到 WiFi 配置界面重新选择
      Serial.println("wifi: auto-connect timeout, entering setup");
      s_wifiConnStart = 0;
      s_inWifiConfig = true;
      wfInit(s_wfCfg);
    }
  } else if (WiFi.status() != WL_CONNECTED) {
    s_wifiConnected = false;
    WiFi.reconnect();
    s_wifiConnStart = millis();  // 断线重连也按 10 秒超时处理
  }

  uint32_t now = millis();

  // PWR 键：已连接时手动刷新；自动连接等待中则直接进入 WiFi 配置
  if (hwBtnA().wasPressed && !s_inWifiConfig) {
    if (s_wifiConnected) {
      s_lastFetch = 0;
    } else if (s_wifiConnStart != 0) {
      s_wifiConnStart = 0;
      s_inWifiConfig = true;
      wfInit(s_wfCfg);
    }
  }

  if (s_wifiConnected && (now - s_lastFetch >= FETCH_INTERVAL_MS || s_lastFetch == 0)) {
    s_lastFetch = now;
    dsFetchToday(s_usage, s_cfg.ds_token);
  }

  // BOOT 键进 WiFi 配置
  if (hwBtnBoot().wasPressed && !s_inWifiConfig) {
    s_inWifiConfig = true;
    wfInit(s_wfCfg);
  }

  if (s_inWifiConfig) {
    bool done = wfTick(s_wfCfg);
    if (s_wfCfg.state == WfState::Connected) {
      s_wifiConnected = true;
      startNtp();
      s_lastFetch = 0;
      s_inWifiConfig = false;
    } else if (done) {
      s_inWifiConfig = false;
    }
    wfDraw(s_wfCfg);
  } else if (s_wifiConnected) {
    usageDisplayDrawDs(s_usage, WiFi.localIP().toString().c_str());
  } else {
    spr.fillScreen(MAIN_BG);
    spr.setFont(u8g2_font_wqy14_t_gb2312b);
    spr.setTextColor(MAIN_YELLOW);
    spr.setCursor(SAFE_L, 100);
    spr.print("正在连接 WiFi...");
  }

  hwDisplayPush();
  delay(16);
}
