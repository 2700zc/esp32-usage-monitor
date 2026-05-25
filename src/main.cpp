#define spr (*hwCanvas())

#include "config.h"
#include "api_client.h"
#include "usage_display.h"
#include "hw/hw.h"
#include "hw/net.h"
#include "voice/i2s_audio.h"
#include "hw/imu.h"
#include <math.h>
#include <WiFi.h>
#include <time.h>
#include <LittleFS.h>

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

static int s_easterState = 0;
static uint32_t s_easterStart = 0;
static File s_easterFile;
static uint32_t s_easterTotal = 0;
static uint32_t s_easterWritten = 0;

extern "C" void handleHttpStatus() {
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
        Serial.println("state=running");
    } else if (req.indexOf("state=done") >= 0 || req.indexOf("state=failed") >= 0) {
        s_thinking = false;
        Serial.println("state=done/failed");
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
    Serial.begin(115200);
    delay(100);
    Serial.println("ESP32 Usage Monitor");

    hwInit();
    loadConfig(s_cfg);
    bool audioOk = audioInit();

    if (audioOk) {
        // Boot test tone: 440 Hz sine, 1 second
        uint8_t toneBuf[512];
        float phase = 0;
        uint32_t total = 0;
        const uint32_t target = 32000;
        if (audioTxStart()) {
            while (total < target) {
                size_t n = 512;
                if (total + n > target) n = target - total;
                for (size_t i = 0; i < n; i += 2) {
                    int16_t s = (int16_t)(8000 * sin(phase));
                    toneBuf[i] = s & 0xFF;
                    toneBuf[i + 1] = (s >> 8) & 0xFF;
                    phase += 2 * PI * 440.0 / 16000.0;
                    if (phase > 2 * PI) phase -= 2 * PI;
                }
                audioTxWrite(toneBuf, n);
                total += n;
            }
            audioTxStop();
            Serial.println("boot tone done");
        }
    }

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

    uint32_t now = millis();

    if (now - s_lastFetch >= 60000 || s_lastFetch == 0) {
        s_lastFetch = now;
        apiFetchUsage(s_usage, s_cfg.server_id, s_cfg.cookie, s_cfg.workspace_id);
    }

    uint32_t ntpInterval = s_timeValid ? 3600000 : 5000;
    if (s_ntpStarted && now - s_lastRtcSync >= ntpInterval) {
        s_lastRtcSync = now;
        syncNtp();
    }

    // Shake detection
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
            Serial.println("Easter: shake detected!");
            stopEaster();
            s_thinking = false;
            s_easterFile = LittleFS.open("/555_audio.raw", "r");
            if (s_easterFile) {
                s_easterTotal = s_easterFile.size();
                s_easterWritten = 0;
                s_easterStart = now;
                s_easterState = 1;
                audioTxStart();
                Serial.printf("Easter: playing %u bytes\n", s_easterTotal);
            }
        }
    }

    // Easter egg audio streaming
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
            Serial.println("Easter: done");
        }
    }

    // PWR / KEY1: stop easter or dismiss thinking
    if (hwBtnA().wasPressed) {
        if (s_easterState == 1) {
            stopEaster();
        }
        if (s_thinking) {
            s_thinking = false;
        } else {
            s_showTime = !s_showTime;
        }
    }

    // Thinking auto-dismiss: removed per user request — only state=done clears it

    // Draw
    if (s_easterState == 1) {
        usageDisplayDrawEaster555(now - s_easterStart);
    } else if (s_thinking) {
        usageDisplayDrawThinking(now - s_thinkingSince);
    } else {
        if (s_showTime) {
            usageDisplayDrawTime(WiFi.localIP().toString().c_str(), s_timeValid);
        } else {
            usageDisplayDraw(s_usage, WiFi.localIP().toString().c_str());
        }
    }

    hwDisplayPush();
    delay(16);
}
