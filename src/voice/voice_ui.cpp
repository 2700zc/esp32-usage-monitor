#include "voice_ui.h"
#include "hw/display.h"
#include <U8g2lib.h>
#include <cstdio>
#include <cstring>

#define spr (*hwCanvas())

static const uint16_t COL_BG    = 0x0000;
static const uint16_t COL_TEXT  = 0xFFFF;
static const uint16_t COL_DIM   = 0x4208;
static const uint16_t COL_GREEN = 0x07E0;
static const uint16_t COL_YELLOW= 0xFFE0;
static const uint16_t COL_RED   = 0xF800;
static const uint16_t COL_CYAN  = 0x07FF;

static void drawWsIndicator(bool connected, int x, int y) {
    spr.setFont(u8g2_font_wqy12_t_gb2312b);
    spr.setCursor(x, y);
    if (connected) {
        spr.setTextColor(COL_GREEN);
        spr.print("WS:OK");
    } else {
        spr.setTextColor(COL_RED);
        spr.print("WS:X");
    }
}

static void drawMicIcon(int cx, int cy) {
    spr.fillCircle(cx, cy, 12, COL_CYAN);
    spr.fillCircle(cx, cy, 8, COL_BG);
    spr.fillCircle(cx, cy - 4, 5, COL_CYAN);
    spr.fillRect(cx - 2, cy, 4, 10, COL_CYAN);
}

static void drawBrainIcon(int cx, int cy) {
    spr.drawCircle(cx, cy, 10, COL_YELLOW);
    spr.drawCircle(cx - 4, cy - 3, 4, COL_YELLOW);
    spr.drawCircle(cx + 4, cy - 3, 4, COL_YELLOW);
    spr.drawLine(cx - 6, cy + 4, cx - 4, cy + 8, COL_YELLOW);
    spr.drawLine(cx + 6, cy + 4, cx + 4, cy + 8, COL_YELLOW);
}

static void formatElapsed(char* buf, size_t sz, uint32_t ms) {
    uint32_t sec = ms / 1000;
    uint32_t m = sec / 60;
    uint32_t s = sec % 60;
    snprintf(buf, sz, "%02lu:%02lu", m, s);
}

void voiceUiDrawIdle(const char* ip, bool wsConnected) {
    spr.fillScreen(COL_BG);

    spr.setFont(u8g2_font_wqy16_t_gb2312b);
    spr.setTextColor(COL_CYAN);
    spr.setCursor(SAFE_L, 40);
    spr.print("VOICE");

    spr.setFont(u8g2_font_wqy12_t_gb2312b);
    spr.setTextColor(COL_DIM);
    spr.setCursor(SAFE_L, 65);
    spr.print("Press BOOT to record");

    drawWsIndicator(wsConnected, SAFE_L, 90);

    if (ip && ip[0]) {
        spr.setTextColor(COL_YELLOW);
        spr.setCursor(SAFE_L, 120);
        spr.printf("IP: %s", ip);
    }
}

void voiceUiDrawListening(uint32_t elapsedMs, bool wsConnected) {
    spr.fillScreen(COL_BG);

    drawMicIcon(SAFE_L + 20, 40);

    spr.setFont(u8g2_font_wqy16_t_gb2312b);
    spr.setTextColor(COL_TEXT);
    spr.setCursor(SAFE_L + 45, 45);
    spr.print("Listening...");

    char timeBuf[16];
    formatElapsed(timeBuf, sizeof(timeBuf), elapsedMs);
    spr.setFont(u8g2_font_wqy14_t_gb2312b);
    spr.setTextColor(COL_GREEN);
    spr.setCursor(SAFE_L, 80);
    spr.print(timeBuf);

    drawWsIndicator(wsConnected, SAFE_L, 110);

    uint8_t barCount = (elapsedMs / 300) % 5 + 1;
    spr.setFont(u8g2_font_wqy12_t_gb2312b);
    spr.setTextColor(COL_DIM);
    spr.setCursor(SAFE_L, 140);
    for (int i = 0; i < barCount; i++) {
        spr.print("| ");
    }
}

void voiceUiDrawProcessing() {
    spr.fillScreen(COL_BG);

    drawBrainIcon(SAFE_L + SAFE_W / 2, 60);

    spr.setFont(u8g2_font_wqy16_t_gb2312b);
    spr.setTextColor(COL_YELLOW);
    int tw = SAFE_W;
    spr.setCursor(SAFE_L + (tw - 84) / 2, 100);
    spr.print("Processing...");
}

void voiceUiDrawResult(const char* text) {
    spr.fillScreen(COL_BG);

    spr.setFont(u8g2_font_wqy16_t_gb2312b);
    spr.setTextColor(COL_GREEN);
    spr.setCursor(SAFE_L, 30);
    spr.print("Done");

    spr.setFont(u8g2_font_wqy12_t_gb2312b);
    spr.setTextColor(COL_TEXT);

    if (!text || !text[0]) {
        spr.setCursor(SAFE_L, 60);
        spr.print("(empty)");
        return;
    }

    int x = SAFE_L;
    int y = 60;
    int maxW = SAFE_W;

    while (*text && y < SAFE_B - 12) {
        int charW = 12;
        if ((uint8_t)*text > 0x7F) charW = 12;

        if (x + charW > SAFE_L + maxW) {
            x = SAFE_L;
            y += 16;
        }
        spr.setCursor(x, y);

        uint8_t b1 = (uint8_t)*text;
        if (b1 >= 0xE0) {
            char c[4] = {text[0], text[1], text[2], 0};
            spr.print(c);
            text += 3;
            x += 12;
        } else if (b1 >= 0xC0) {
            char c[3] = {text[0], text[1], 0};
            spr.print(c);
            text += 2;
            x += 12;
        } else {
            spr.print(*text);
            text++;
            x += 6;
        }
    }
}

void voiceUiDrawNoConfig() {
    spr.fillScreen(COL_BG);

    spr.setFont(u8g2_font_wqy14_t_gb2312b);
    spr.setTextColor(COL_RED);
    spr.setCursor(SAFE_L, 40);
    spr.print("No PC Host");

    spr.setFont(u8g2_font_wqy12_t_gb2312b);
    spr.setTextColor(COL_DIM);
    spr.setCursor(SAFE_L, 65);
    spr.print("Set pc_host in");
    spr.setCursor(SAFE_L, 82);
    spr.print("config.json");

    spr.setCursor(SAFE_L, 110);
    spr.setTextColor(COL_YELLOW);
    spr.print("Double-click BOOT");
    spr.setCursor(SAFE_L, 125);
    spr.print("to switch back");
}