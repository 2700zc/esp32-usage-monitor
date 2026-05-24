#define spr (*hwCanvas())

#include "config.h"
#include "api_client.h"
#include "usage_display.h"
#include "hw/hw.h"
#include "hw/net.h"
#include "voice/i2s_audio.h"
#include "voice/ws_client.h"
#include "voice/voice_ui.h"
#include <WiFi.h>
#include <time.h>

enum AppMode { MODE_MONITOR, MODE_VOICE };
enum VoiceState { VOICE_IDLE, VOICE_RECORDING, VOICE_PROCESSING, VOICE_DONE };

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

static AppMode s_mode = MODE_MONITOR;
static VoiceState s_voiceState = VOICE_IDLE;
static uint32_t s_recordStart = 0;
static uint32_t s_doneStart = 0;
static char s_resultText[256] = {};
static uint32_t s_lastBootPress = 0;
static bool s_doubleClickPending = false;
static bool s_wsConnected = false;
static volatile bool s_resultReady = false;

static void onWsResult(const char* text) {
    strlcpy(s_resultText, text, sizeof(s_resultText));
    s_resultReady = true;
}

extern "C" void handleHttpStatus() {
    WiFiClient client = s_server.accept();
    if (client) {
        String req;
        while (client.connected()) {
            if (client.available()) {
                String line = client.readStringUntil('\n');
                req += line;
                if (line == "\r") break;
            }
        }
        if (req.indexOf("state=running") >= 0) {
            s_thinking = true;
            s_thinkingSince = millis();
        } else if (req.indexOf("state=done") >= 0 || req.indexOf("state=failed") >= 0) {
            s_thinking = false;
        }
        client.println("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nOK");
        delay(10);
        client.stop();
    }
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
    Serial.begin(115200);
    delay(100);
    Serial.println("ESP32 Usage Monitor + Voice");

    hwInit();
    loadConfig(s_cfg);
    audioInit();
    wsSetResultCallback(onWsResult);

    char savedSSID[33], savedPass[65];
    bool hasCreds = netLoadCred(savedSSID, 33, savedPass, 65);

    if (hasCreds) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(savedSSID, savedPass);
    }

    for (;;) {
        if (WiFi.status() == WL_CONNECTED) {
            syncNtp();
            s_server.begin();
            Serial.println("WiFi connected");
            break;
        }
        delay(500);
    }
}

void loop() {
    hwInputUpdate();

    handleHttpStatus();

    uint32_t now = millis();

    if (s_mode == MODE_MONITOR) {
        if (now - s_lastFetch >= 60000 || s_lastFetch == 0) {
            s_lastFetch = now;
            apiFetchUsage(s_usage, s_cfg.server_id, s_cfg.cookie, s_cfg.workspace_id);
        }

        if (s_ntpStarted && now - s_lastRtcSync >= 3600000) {
            s_lastRtcSync = now;
            syncNtp();
        }
    }

    if (hwBtnBoot().wasPressed) {
        uint32_t pressTime = now;
        if (pressTime - s_lastBootPress < 300) {
            s_doubleClickPending = false;
            s_lastBootPress = 0;
            if (s_mode == MODE_MONITOR) {
                s_mode = MODE_VOICE;
                s_voiceState = VOICE_IDLE;
                if (s_cfg.pc_host[0]) {
                    wsConnect(s_cfg.pc_host, s_cfg.pc_port);
                }
                Serial.println("Mode: VOICE");
            } else {
                s_mode = MODE_MONITOR;
                if (s_voiceState == VOICE_RECORDING) {
                    audioStop();
                    wsSendText("{\"type\":\"stop\"}");
                }
                s_voiceState = VOICE_IDLE;
                wsDisconnect();
                Serial.println("Mode: MONITOR");
            }
        } else {
            s_doubleClickPending = true;
            s_lastBootPress = pressTime;
        }
    }

    if (s_doubleClickPending && now - s_lastBootPress >= 300) {
        s_doubleClickPending = false;
        if (s_mode == MODE_MONITOR) {
            if (s_thinking) s_thinking = false;
        } else {
            if (s_voiceState == VOICE_IDLE) {
                s_voiceState = VOICE_RECORDING;
                s_recordStart = now;
                audioStart();
                wsSendText("{\"type\":\"start\"}");
                Serial.println("Voice: RECORDING");
            } else if (s_voiceState == VOICE_RECORDING) {
                audioStop();
                wsSendText("{\"type\":\"stop\"}");
                s_voiceState = VOICE_PROCESSING;
                Serial.println("Voice: PROCESSING");
            }
        }
    }

    if (s_mode == MODE_VOICE && s_voiceState == VOICE_RECORDING) {
        uint8_t pcmBuf[640];
        size_t n = audioRead(pcmBuf, sizeof(pcmBuf));
        if (n > 0 && wsIsConnected()) {
            wsSendBin(pcmBuf, n);
        }
    }

    wsPoll();
    if (s_resultReady && s_voiceState == VOICE_PROCESSING) {
        s_resultReady = false;
        s_voiceState = VOICE_DONE;
        s_doneStart = now;
        Serial.printf("Voice: DONE text=%s\n", s_resultText);
    }

    if (s_voiceState == VOICE_DONE && now - s_doneStart >= 2000) {
        s_voiceState = VOICE_IDLE;
    }

    if (s_thinking && s_mode == MODE_MONITOR) {
        if (hwBtnBoot().wasPressed || now - s_thinkingSince > 300000) {
            s_thinking = false;
        }
    }

    if (hwBtnA().wasPressed && s_mode == MODE_MONITOR && !s_thinking) {
        s_showTime = !s_showTime;
    }

    if (s_thinking && s_mode == MODE_MONITOR) {
        usageDisplayDrawThinking(s_thinkingSince);
    } else if (s_mode == MODE_MONITOR) {
        s_wsConnected = wsIsConnected();
        if (s_showTime) {
            usageDisplayDrawTime(WiFi.localIP().toString().c_str(), s_timeValid);
        } else {
            usageDisplayDraw(s_usage, WiFi.localIP().toString().c_str());
        }
    } else {
        s_wsConnected = wsIsConnected();
        if (!s_cfg.pc_host[0]) {
            voiceUiDrawNoConfig();
        } else {
            switch (s_voiceState) {
            case VOICE_IDLE:
                voiceUiDrawIdle(WiFi.localIP().toString().c_str(), s_wsConnected);
                break;
            case VOICE_RECORDING:
                voiceUiDrawListening(now - s_recordStart, s_wsConnected);
                break;
            case VOICE_PROCESSING:
                voiceUiDrawProcessing();
                break;
            case VOICE_DONE:
                voiceUiDrawResult(s_resultText);
                break;
            }
        }
    }

    hwDisplayPush();
    delay(16);
}