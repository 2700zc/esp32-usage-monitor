#include "wifi_config.h"
#include "hw/hw.h"
#include "hw/input.h"
#include "hw/display.h"
#include <U8g2lib.h>
#include <WiFi.h>

#define spr (*hwCanvas())

static const uint16_t C_BG    = 0x0000;
static const uint16_t C_ACC   = 0xFFE0;
static const uint16_t C_DIM   = 0x4208;
static const uint16_t C_KEY   = 0x18C6;
static const uint16_t C_SEL   = 0x07FF;
static const uint16_t C_WHITE = 0xFFFF;
static const uint16_t C_RED   = 0xF800;

static const int KW = 14;
static const int KH = 18;
static const int KG = 1;

static const char* const ROWS_ABC[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
static const char* const ROWS_ABC_UP[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
static const char* const ROWS_123[] = {"1234567890", "-/:;()&@\"", ".,?!'  "};

static int rowLen(int row, int mode) {
    if (mode < 2) return strlen(ROWS_ABC[row]);
    return (row < 2) ? strlen(ROWS_123[row]) : 8;
}

static char rowChar(int row, int col, int mode) {
    if (mode == 0) return ROWS_ABC[row][col];
    if (mode == 1) return ROWS_ABC_UP[row][col];
    return ROWS_123[row][col];
}

static int totalKeyCols(int row, int mode) {
    if (row < 3) return rowLen(row, mode);
    return 4; // bottom row: mode, shift, space, backspace, ok
}

void wfInit(WfConfig& cfg) {
    cfg.state = WfState::Scanning;
    cfg.networkCount = 0;
    cfg.listSel = 0;
    cfg.scrollOff = 0;
    cfg.passwordLen = 0;
    cfg.password[0] = 0;
    cfg.kbMode = 0;
    cfg.curRow = 0;
    cfg.curCol = 0;
    cfg.ssid[0] = 0;
    cfg.connectStart = 0;
}

static void doScan(WfConfig& cfg) {
    spr.fillScreen(C_BG);
    spr.setFont(u8g2_font_wqy16_t_gb2312b);
    spr.setTextColor(C_ACC);
    spr.setCursor(SAFE_L, SAFE_T + 100);
    spr.print("WiFi...");
    hwDisplayPush();
    // 停止旧的自动连接/重连循环，否则 scanNetworks 会被干扰而失败（空列表）
    WiFi.disconnect(true);
    delay(100);
    cfg.networkCount = netScan(cfg.networks, 8);
    cfg.listSel = 0;
    cfg.scrollOff = 0;
    cfg.state = WfState::ScanList;
}

static void drawScanList(WfConfig& cfg) {
    spr.fillScreen(C_BG);
    spr.setFont(u8g2_font_wqy16_t_gb2312b);
    spr.setTextColor(C_ACC);
    spr.setCursor(SAFE_L, SAFE_T + 4);
    spr.print("WiFi");

    if (cfg.networkCount == 0) {
        spr.setFont(u8g2_font_wqy12_t_gb2312b);
        spr.setTextColor(C_DIM);
        spr.setCursor(SAFE_L, SAFE_T + 30);
        spr.print("not found");
        spr.setCursor(SAFE_L, SAFE_T + 50);
        spr.print("PWR: rescan");
        return;
    }

    spr.setFont(u8g2_font_6x10_tf);
    spr.setTextColor(C_DIM);
    spr.setCursor(SAFE_R - 30, SAFE_T + 6);
    spr.printf("%d/%d", cfg.listSel + 1, cfg.networkCount);

    int y0 = SAFE_T + 24;
    int visN = 7;
    if (cfg.listSel < cfg.scrollOff) cfg.scrollOff = cfg.listSel;
    if (cfg.listSel >= cfg.scrollOff + visN) cfg.scrollOff = cfg.listSel - visN + 1;

    for (int i = 0; i < visN; i++) {
        int idx = cfg.scrollOff + i;
        if (idx >= cfg.networkCount) break;
        int y = y0 + i * 16;
        bool sel = (idx == cfg.listSel);
        if (sel) {
            spr.fillRect(SAFE_L, y - 1, SAFE_W, 15, C_SEL);
            spr.setTextColor(0x0000);
        } else {
            spr.setTextColor(C_WHITE);
        }
        spr.setCursor(SAFE_L + 2, y + 10);
        spr.printf("%.24s", cfg.networks[idx].ssid);
        if (cfg.networks[idx].encryption != WIFI_AUTH_OPEN) {
            spr.setCursor(SAFE_R - 10, y + 10);
            spr.print("*");
        }
    }

    spr.setFont(u8g2_font_wqy12_t_gb2312b);
    spr.setTextColor(C_DIM);
    spr.setCursor(SAFE_L, SAFE_B - 14);
    spr.print("PWR:\xe2\x86\x95 BOOT:\xe2\x86\xb5");
}

static void drawKeyboard(WfConfig& cfg) {
    spr.fillScreen(C_BG);

    spr.setFont(u8g2_font_6x10_tf);
    spr.setTextColor(C_DIM);
    spr.setCursor(SAFE_L, SAFE_T + 3);
    spr.printf("%.28s", cfg.ssid);
    spr.setTextColor(C_ACC);
    spr.setCursor(SAFE_L, SAFE_T + 15);
    for (int i = 0; i < cfg.passwordLen && i < 18; i++) spr.print('*');
    spr.print('_');

    spr.drawLine(SAFE_L, SAFE_T + 23, SAFE_R, SAFE_T + 23, C_DIM);

    int ky0 = SAFE_T + 27;
    const char* labels3[] = {"num", "abc", "ABC"};
    const char* modeLabel = labels3[cfg.kbMode];

    for (int r = 0; r < 3; r++) {
        int n = rowLen(r, cfg.kbMode);
        int rw = KW * n + KG * (n - 1);
        int rx = SAFE_L + (SAFE_W - rw) / 2;
        for (int c = 0; c < n; c++) {
            int kx = rx + c * (KW + KG);
            int ky = ky0 + r * (KH + KG);
            bool cur = (cfg.curRow == r && cfg.curCol == c);
            uint16_t bg = cur ? C_SEL : C_KEY;
            uint16_t fg = cur ? 0x0000 : C_WHITE;
            spr.fillRect(kx, ky, KW, KH, bg);
            spr.drawRect(kx, ky, KW, KH, fg);
            spr.setFont(u8g2_font_6x10_tf);
            spr.setTextColor(fg);
            char ch[2] = {rowChar(r, c, cfg.kbMode), 0};
            if (cfg.kbMode == 2 && r == 2 && ch[0] == ' ') ch[0] = '_';
            spr.setCursor(kx + (KW - 6) / 2, ky + (KH - 10) / 2 + 9);
            spr.print(ch);
        }
    }

    int by = ky0 + 3 * (KH + KG);
    struct BKey { const char* label; int w; };
    BKey bkeys[] = {
        {modeLabel, 22},
        {"del", 22},
        {" ", 44},
        {"OK", 30}
    };
    int bx = SAFE_L;
    for (int i = 0; i < 4; i++) {
        bool cur = (cfg.curRow == 3 && cfg.curCol == i);
        uint16_t bg = cur ? C_SEL : (i == 3 ? C_ACC : C_DIM);
        uint16_t fg = cur ? 0x0000 : C_WHITE;
        spr.fillRect(bx, by, bkeys[i].w, KH, bg);
        spr.drawRect(bx, by, bkeys[i].w, KH, fg);
        spr.setFont(u8g2_font_6x10_tf);
        spr.setTextColor(fg);
        spr.setCursor(bx + (bkeys[i].w - 6 * strlen(bkeys[i].label)) / 2, by + (KH - 10) / 2 + 9);
        spr.print(bkeys[i].label);
        bx += bkeys[i].w + KG;
    }

    spr.setFont(u8g2_font_wqy12_t_gb2312b);
    spr.setTextColor(C_DIM);
    spr.setCursor(SAFE_L, SAFE_B - 2);
    spr.print("PWR:\xe2\x86\x90\xe2\x86\x92 Key2:\xe2\x86\x91\xe2\x86\x93");
}

static void drawConnecting(WfConfig& cfg) {
    spr.fillScreen(C_BG);
    spr.setFont(u8g2_font_wqy16_t_gb2312b);
    spr.setTextColor(C_ACC);
    spr.setCursor(SAFE_L, SAFE_T + 100);
    spr.print("connecting");
    uint32_t d = (millis() / 400) % 4;
    for (uint32_t i = 0; i < d; i++) spr.print('.');
}

static void drawConnected(WfConfig& cfg) {
    spr.fillScreen(C_BG);
    spr.setFont(u8g2_font_wqy16_t_gb2312b);
    spr.setTextColor(C_ACC);
    spr.setCursor(SAFE_L, SAFE_T + 80);
    spr.print("OK!");
    spr.setFont(u8g2_font_6x10_tf);
    spr.setTextColor(C_WHITE);
    spr.setCursor(SAFE_L, SAFE_T + 110);
    spr.print(WiFi.localIP().toString().c_str());
}

static void drawFailed(WfConfig& cfg) {
    spr.fillScreen(C_BG);
    spr.setFont(u8g2_font_wqy16_t_gb2312b);
    spr.setTextColor(C_RED);
    spr.setCursor(SAFE_L, SAFE_T + 80);
    spr.print("failed");
    spr.setFont(u8g2_font_wqy12_t_gb2312b);
    spr.setTextColor(C_DIM);
    spr.setCursor(SAFE_L, SAFE_T + 110);
    spr.print("BOOT: retry");
}

void wfDraw(WfConfig& cfg) {
    switch (cfg.state) {
        case WfState::Scanning:   drawScanList(cfg); break;
        case WfState::ScanList:   drawScanList(cfg); break;
        case WfState::Password:   drawKeyboard(cfg); break;
        case WfState::Connecting: drawConnecting(cfg); break;
        case WfState::Connected:  drawConnected(cfg); break;
        case WfState::Failed:      drawFailed(cfg); break;
    }
}

bool wfTick(WfConfig& cfg) {
    if (cfg.state == WfState::Scanning) {
        doScan(cfg);
        return false;
    }

    if (cfg.state == WfState::Connecting) {
        if (WiFi.status() == WL_CONNECTED) {
            netSaveCred(cfg.ssid, cfg.password);
            cfg.connectStart = 0;
            cfg.state = WfState::Connected;
        } else if (millis() - cfg.connectStart > 15000) {
            cfg.state = WfState::Failed;
        }
        return false;
    }

    if (cfg.state == WfState::Connected) {
        if (!cfg.connectStart) cfg.connectStart = millis();
        if (millis() - cfg.connectStart > 2000) return true;
        return false;
    }

    const HwBtn& a = hwBtnA();
    const HwBtn& b = hwBtnB();
    const HwBtn& boot = hwBtnBoot();

    if (cfg.state == WfState::ScanList) {
        if (a.wasPressed) {
            if (cfg.networkCount == 0) {
                cfg.state = WfState::Scanning;  // 空列表：按 PWR 重新扫描
                return false;
            }
            cfg.listSel++;
            if (cfg.listSel >= cfg.networkCount) cfg.listSel = 0;
        }
        if (b.wasPressed) {
            cfg.listSel--;
            if (cfg.listSel < 0) cfg.listSel = cfg.networkCount - 1;
        }
        if (boot.wasPressed && cfg.networkCount > 0) {
            strlcpy(cfg.ssid, cfg.networks[cfg.listSel].ssid, sizeof(cfg.ssid));
            cfg.passwordLen = 0;
            cfg.password[0] = 0;
            cfg.kbMode = 0;
            cfg.curRow = 0;
            cfg.curCol = 0;
            cfg.state = WfState::Password;
        }
        return false;
    }

    if (cfg.state == WfState::Password) {
        int maxCols[4];
        for (int r = 0; r < 3; r++) maxCols[r] = rowLen(r, cfg.kbMode) - 1;
        maxCols[3] = 3; // num, del, space, ok

        if (a.wasPressed) {
            cfg.curCol++;
            if (cfg.curRow < 3 && cfg.curCol > maxCols[cfg.curRow]) {
                cfg.curCol = 0;
                cfg.curRow++;
                if (cfg.curRow > 3) cfg.curRow = 0;
            }
            if (cfg.curRow == 3 && cfg.curCol > 3) {
                cfg.curCol = 0;
                cfg.curRow++;
                if (cfg.curRow > 3) cfg.curRow = 0;
            }
        }
        if (b.wasPressed) {
            cfg.curCol--;
            if (cfg.curCol < 0) {
                cfg.curRow--;
                if (cfg.curRow < 0) cfg.curRow = 3;
                cfg.curCol = maxCols[cfg.curRow];
            }
        }
        if (a.pressedFor(500)) {
            static uint32_t lastRep = 0;
            if (millis() - lastRep > 120) {
                lastRep = millis();
                cfg.curCol++;
                if (cfg.curRow < 3 && cfg.curCol > maxCols[cfg.curRow]) {
                    cfg.curCol = 0;
                    cfg.curRow++;
                    if (cfg.curRow > 3) cfg.curRow = 0;
                }
                if (cfg.curRow == 3 && cfg.curCol > 3) {
                    cfg.curCol = 0;
                    cfg.curRow++;
                    if (cfg.curRow > 3) cfg.curRow = 0;
                }
            }
        }
        if (b.pressedFor(500)) {
            static uint32_t lastRep2 = 0;
            if (millis() - lastRep2 > 120) {
                lastRep2 = millis();
                cfg.curCol--;
                if (cfg.curCol < 0) {
                    cfg.curRow--;
                    if (cfg.curRow < 0) cfg.curRow = 3;
                    cfg.curCol = maxCols[cfg.curRow];
                }
            }
        }

        if (boot.wasPressed) {
            if (cfg.curRow < 3) {
                char ch = rowChar(cfg.curRow, cfg.curCol, cfg.kbMode);
                if (cfg.passwordLen < 63 && ch != ' ') {
                    cfg.password[cfg.passwordLen++] = ch;
                    cfg.password[cfg.passwordLen] = 0;
                }
            } else {
                switch (cfg.curCol) {
                    case 0: // mode toggle
                        cfg.kbMode = (cfg.kbMode + 1) % 3;
                        cfg.curCol = 0;
                        break;
                    case 1: // del
                        if (cfg.passwordLen > 0)
                            cfg.password[--cfg.passwordLen] = 0;
                        break;
                    case 2: // space
                        if (cfg.passwordLen < 63) {
                            cfg.password[cfg.passwordLen++] = ' ';
                            cfg.password[cfg.passwordLen] = 0;
                        }
                        break;
                    case 3: // OK - connect
                        cfg.state = WfState::Connecting;
                        cfg.connectStart = millis();
                        WiFi.mode(WIFI_STA);
                        WiFi.begin(cfg.ssid, cfg.password);
                        break;
                }
            }
        }
        return false;
    }

    if (cfg.state == WfState::Failed) {
        if (boot.wasPressed) {
            cfg.state = WfState::Scanning;
        }
        return false;
    }

    return false;
}