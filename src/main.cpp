#define spr (*hwCanvas())

#include "config.h"
#include "api_client.h"
#include "usage_display.h"
#include "hw/hw.h"
#include "hw/net.h"
#include "voice/i2s_audio.h"
#include "voice/stt.h"
#include "hw/imu.h"
#include "wifi_config.h"
#include <ESPmDNS.h>
#include <math.h>
#include <WiFi.h>
#include <time.h>
#include <LittleFS.h>
#include <U8g2lib.h>

static const uint16_t MAIN_BG     = 0x0000;
static const uint16_t MAIN_YELLOW = 0xFFE0;
static const uint16_t MAIN_DIM    = 0x4208;

static UsageData s_usage;
static AppConfig s_cfg;
static uint32_t s_lastFetch = 0;
static bool s_showTime = false;
static bool s_ntpStarted = false;
static bool s_timeValid = false;
static uint32_t s_lastRtcSync = 0;
static WiFiServer s_server(80);
static bool s_thinking = false;
static uint32_t s_thinkingSince = 0;

static int s_thinkingStep = 0;
static char s_thinkingMsg[64] = "";
static int s_totalSteps = 0;
static bool s_showDone = false;
static bool s_showFailed = false;
static uint32_t s_doneSince = 0;

static int s_easterState = 0;
static uint32_t s_easterStart = 0;
static File s_easterFile;
static uint32_t s_easterTotal = 0;
static uint32_t s_easterWritten = 0;

static bool s_wifiConnected = false;
static char s_ssid[33] = "";
static WfConfig s_wfCfg;
static bool s_inWifiConfig = false;

static SttContext s_stt;

static void urlDecode(char* dst, const char* src, size_t dstSize) {
  size_t di = 0;
  for (size_t si = 0; src[si] && di < dstSize - 1; si++) {
    if (src[si] == '%' && src[si+1] && src[si+2]) {
      char hex[3] = { src[si+1], src[si+2], 0 };
      dst[di++] = (char)strtol(hex, nullptr, 16);
      si += 2;
    } else if (src[si] == '+') {
      dst[di++] = ' ';
    } else {
      dst[di++] = src[si];
    }
  }
  dst[di] = 0;
}

extern "C" void handleHttpStatus() {
    if (!s_wifiConnected) return;
    WiFiClient client = s_server.accept();
    if (!client) return;

    client.setTimeout(50);

    uint32_t t0 = millis();
    while (!client.available() && (millis() - t0) < 100) {
        delay(1);
    }

    String req;
    for (int i = 0; i < 15; i++) {
        if (!client.connected() && !client.available()) break;
        if (!client.available()) break;
        String line = client.readStringUntil('\n');
        req += line;
        if (line == "\r") break;
    }

    if (req.indexOf("state=running") >= 0) {
        s_thinking = true;
        s_thinkingSince = millis();
        s_thinkingStep = 0;
        s_thinkingMsg[0] = 0;

        int stepIdx = req.indexOf("step=");
        if (stepIdx >= 0) {
            s_thinkingStep = atoi(req.c_str() + stepIdx + 5);
        }

        int msgIdx = req.indexOf("msg=");
        if (msgIdx >= 0) {
            const char* start = req.c_str() + msgIdx + 4;
            char raw[64] = "";
            int ri = 0;
            while (*start && *start != ' ' && *start != '&' && ri < 63) {
                raw[ri++] = *start++;
            }
            raw[ri] = 0;
            urlDecode(s_thinkingMsg, raw, sizeof(s_thinkingMsg));
        }

    } else if (req.indexOf("state=done") >= 0) {
        if (s_thinking) {
            s_showDone = true;
            s_doneSince = millis();
        }
        s_thinking = false;

    } else if (req.indexOf("state=failed") >= 0) {
        if (s_thinking) {
            s_showFailed = true;
            s_doneSince = millis();
        }
        s_thinking = false;
    }

    client.println("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nOK");
    delay(10);
    client.stop();
}

static void syncNtp() {
    if (!s_ntpStarted) {
        configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
        s_ntpStarted = true;
    }
    time_t ts = time(nullptr);
    if (ts > 100000) {
        struct tm lt;
        localtime_r(&ts, &lt);
        HwTime rt = {
            (uint8_t)lt.tm_hour, (uint8_t)lt.tm_min, (uint8_t)lt.tm_sec,
            (uint16_t)(lt.tm_year + 1900),
            (uint8_t)(lt.tm_mon + 1), (uint8_t)lt.tm_mday, (uint8_t)lt.tm_wday
        };
        hwRtcWrite(rt);
        s_timeValid = true;
    }
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
    audioInit();
    sttInit(s_stt);

    char savedSSID[33], savedPass[65];
    bool hasCreds = netLoadCred(savedSSID, 33, savedPass, 65);
    if (hasCreds) {
        strlcpy(s_ssid, savedSSID, sizeof(s_ssid));
        WiFi.mode(WIFI_STA);
        WiFi.begin(savedSSID, savedPass);
    } else {
        s_inWifiConfig = true;
        wfInit(s_wfCfg);
    }
}

static void stopEaster() {
    if (s_easterState == 1) {
        s_easterFile.close();
        audioTxStop();
        s_easterState = 0;
    }
}

void loop() {
    hwInputUpdate();
    handleHttpStatus();

    if (!s_wifiConnected) {
        if (WiFi.status() == WL_CONNECTED) {
            s_wifiConnected = true;
            syncNtp();
            s_server.begin();
            MDNS.begin("esp32-monitor");
        }
    } else if (WiFi.status() != WL_CONNECTED) {
        s_wifiConnected = false;
        WiFi.reconnect();
    }

    uint32_t now = millis();

    if (s_wifiConnected && (now - s_lastFetch >= 60000 || s_lastFetch == 0)) {
        s_lastFetch = now;
        apiFetchUsage(s_usage, s_cfg.server_id, s_cfg.cookie, s_cfg.workspace_id);
    }

    uint32_t ntpInterval = s_timeValid ? 3600000 : 5000;
    if (s_ntpStarted && now - s_lastRtcSync >= ntpInterval) {
        s_lastRtcSync = now;
        syncNtp();
    }

    if (s_easterState == 0 && !s_thinking) {
        float ax, ay, az;
        hwImuAccel(&ax, &ay, &az);
        float mag2 = ax*ax + ay*ay + az*az;
        static uint8_t shakeCount = 0;
        if (mag2 > 4.0f) {
            shakeCount++;
        } else {
            shakeCount = 0;
        }
        if (shakeCount >= 3) {
            shakeCount = 0;
            stopEaster();
            s_thinking = false;
            s_easterFile = LittleFS.open("/555_audio.raw", "r");
            if (s_easterFile) {
                s_easterTotal = s_easterFile.size();
                s_easterWritten = 0;
                s_easterStart = now;
                s_easterState = 1;
                audioTxStart();
            }
        }
    }

    if (s_easterState == 1) {
        uint8_t buf[512];
        size_t toRead = sizeof(buf);
        if (s_easterTotal - s_easterWritten < toRead)
            toRead = s_easterTotal - s_easterWritten;
        if (toRead > 0) {
            size_t n = s_easterFile.read(buf, toRead);
            audioTxWrite(buf, n);
            s_easterWritten += n;
        }
        if (s_easterWritten >= s_easterTotal) {
            s_easterFile.close();
            audioTxStop();
            s_easterState = 0;
        }
    }

    if (hwBtnA().wasPressed) {
        if (s_easterState == 1) {
            stopEaster();
        } else if (!s_inWifiConfig) {
            if (s_showDone || s_showFailed) {
                s_showDone = false;
                s_showFailed = false;
            } else if (s_thinking) {
                s_thinking = false;
            } else if (!s_wifiConnected) {
                s_inWifiConfig = true;
                wfInit(s_wfCfg);
            } else {
                s_showTime = !s_showTime;
            }
        }
    }

    if (hwBtnBoot().wasPressed && !s_inWifiConfig && s_wifiConnected) {
        s_inWifiConfig = true;
        wfInit(s_wfCfg);
    }

    // ── STT state machine ──────────────────────────────────
    if (s_wifiConnected && s_cfg.pc_host[0] != '\0' && s_cfg.pc_port != 0) {
        if (hwBtnB().wasPressed && s_stt.state == SttState::Idle && s_easterState == 0 && !s_thinking) {
            sttStartRecording(s_stt);
        } else if (hwBtnB().wasPressed && s_stt.state == SttState::Recording) {
            sttStopRecording(s_stt, s_cfg.pc_host, s_cfg.pc_port);
        }
    }

    if (s_stt.state == SttState::Recording) {
        sttTick(s_stt);
        uint32_t elapsed = millis() - s_stt.stateSince;
        if (elapsed >= SttContext::MAX_RECORD_MS) {
            sttStopRecording(s_stt, s_cfg.pc_host, s_cfg.pc_port);
        }
    }

    if ((s_stt.state == SttState::Success || s_stt.state == SttState::Failed) &&
        millis() - s_stt.stateSince >= 3000) {
        sttReset(s_stt);
    }

    // Draw
    if (s_inWifiConfig) {
        bool done = wfTick(s_wfCfg);
        if (s_wfCfg.state == WfState::Connected) {
            s_wifiConnected = true;
            strlcpy(s_ssid, s_wfCfg.ssid, sizeof(s_ssid));
            syncNtp();
            s_server.begin();
            MDNS.begin("esp32-monitor");
            s_inWifiConfig = false;
        } else if (done) {
            s_inWifiConfig = false;
        }
        wfDraw(s_wfCfg);
    } else if (s_easterState == 1) {
        usageDisplayDrawEaster555(now - s_easterStart);
    } else if (s_showDone) {
        usageDisplayDrawDone(s_thinkingStep, now - s_doneSince);
        if (now - s_doneSince >= 2000) {
            s_showDone = false;
        }
    } else if (s_showFailed) {
        usageDisplayDrawFailed(now - s_doneSince);
        if (now - s_doneSince >= 2000) {
            s_showFailed = false;
        }
    } else if (s_thinking) {
        usageDisplayDrawThinking(now - s_thinkingSince, s_thinkingStep, s_thinkingMsg);
    } else if (s_stt.state == SttState::Recording) {
        uint32_t elapsed = millis() - s_stt.stateSince;
        usageDisplayDrawRecorderRecording(elapsed);
    } else if (s_stt.state == SttState::Uploading) {
        usageDisplayDrawRecorderUploading();
    } else if (s_stt.state == SttState::Success) {
        usageDisplayDrawRecorderDone(s_stt.resultText, millis() - s_stt.stateSince);
    } else if (s_stt.state == SttState::Failed) {
        usageDisplayDrawRecorderFailed(millis() - s_stt.stateSince);
    } else if (s_wifiConnected) {
        if (s_showTime) {
            usageDisplayDrawTime(WiFi.localIP().toString().c_str(), s_timeValid);
        } else {
            usageDisplayDraw(s_usage, WiFi.localIP().toString().c_str());
        }
    }

    hwDisplayPush();
    delay(16);
}