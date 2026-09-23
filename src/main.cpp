#define spr (*hwCanvas())

#include "config.h"
#include "deepseek_client.h"
#include "opencode_client.h"
#include "commandcode_client.h"
#include "usage_display.h"
#include "hw/hw.h"
#include "hw/net.h"
#include "hw/gesture.h"
#include "wifi_config.h"
#include <WiFi.h>
#include <time.h>
#include <U8g2lib.h>

static const uint16_t MAIN_BG     = 0x0000;
static const uint16_t MAIN_YELLOW = 0xFFE0;

// 一轮刷新 = 把所有已配置的服务商抓一遍。三家分摊到连续的多帧里做，每帧只抓
// 一个：最长无响应时间就等于单个服务商（约 1.5 秒），帧间照常重绘并轮询触摸，
// 滑动不会长时间失灵。
static const uint32_t REFRESH_ROUND_MS   = 60000;
static const uint32_t CONNECT_TIMEOUT_MS = 10000;  // 自动连接 10 秒超时

static AppConfig        s_cfg;
static OpenCodeUsage    s_oc;
static CommandCodeUsage s_cc;
static DeepSeekUsage    s_ds;

// 页面顺序与 TokenHub 一致：OpenCode Go → CommandCode → DeepSeek。
// 没配齐凭据的服务商不生成页面（只配了 DeepSeek 就仍是单页，滑动无效果）。
enum class Prov : uint8_t { Oc, Cc, Ds };
static Prov s_pages[3];
static int  s_pageCount = 0;
static int  s_page      = 0;

static uint32_t s_lastRound     = 0;  // 上一轮刷新开始时刻；0 = 尚未刷过
static bool     s_refreshActive = false;
static int      s_refreshIdx    = 0;

static uint32_t s_wifiConnStart = 0;  // 自动连接开始时刻；0 = 未在自动连接
static bool     s_wifiConnected = false;
static WfConfig s_wfCfg;
static bool     s_inWifiConfig  = false;

static void buildPages(const AppConfig& c) {
  s_pageCount = 0;
  if (cfgOcReady(c)) s_pages[s_pageCount++] = Prov::Oc;
  if (cfgCcReady(c)) s_pages[s_pageCount++] = Prov::Cc;
  if (cfgDsReady(c)) s_pages[s_pageCount++] = Prov::Ds;
  if (s_page >= s_pageCount) s_page = 0;
}

static void startNtp() {
  configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
}

static void fetchOne(Prov p) {
  switch (p) {
    case Prov::Oc: ocFetch(s_oc, s_cfg.oc_workspace_id, s_cfg.oc_cookie, s_cfg.oc_server_id); break;
    case Prov::Cc: ccFetch(s_cc, s_cfg.cc_cookie); break;
    case Prov::Ds: dsFetchToday(s_ds, s_cfg.ds_token); break;
  }
}

// 画当前页。netStatus 非空时页头改用它作为状态文案（WiFi 断开时传「网络断开」），
// 优先级高于数据自带的 lastError。
static void drawCurrentPage(const char* ip, const char* netStatus) {
  if (s_pageCount == 0) {
    usageDisplayDrawEmpty(ip);
    return;
  }
  switch (s_pages[s_page]) {
    case Prov::Oc: usageDisplayDrawOc(s_oc, ip, s_page, s_pageCount, netStatus); break;
    case Prov::Cc: usageDisplayDrawCc(s_cc, ip, s_page, s_pageCount, netStatus); break;
    case Prov::Ds: usageDisplayDrawDs(s_ds, ip, s_page, s_pageCount, netStatus); break;
  }
}

// 当前页是否已有可显示的数据——决定断网时是继续显示缓存还是给连接提示
static bool currentPageHasData() {
  if (s_pageCount == 0) return false;
  switch (s_pages[s_page]) {
    case Prov::Oc: return s_oc.valid;
    case Prov::Cc: return s_cc.valid;
    case Prov::Ds: return s_ds.valid;
  }
  return false;
}

// 请求一轮刷新：下一帧起从第一个服务商开始抓
static void requestRefresh() {
  s_lastRound = 0;
  s_refreshActive = false;
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
  buildPages(s_cfg);
  Serial.printf("boot: %d page(s) ready (oc=%d cc=%d ds=%d)\n",
                s_pageCount, (int)cfgOcReady(s_cfg), (int)cfgCcReady(s_cfg),
                (int)cfgDsReady(s_cfg));

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
      requestRefresh();          // 连上就立刻刷一轮
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

#if TOUCH_DEBUG
  // 触摸层的坐标映射从未被 UI 用过，这是确认方向与比例的唯一入口。
  // 在 platformio.ini 打开 -DTOUCH_DEBUG=1 即可。
  {
    const HwTouch& t = hwTouch();
    static int16_t lx = -1, ly = -1;
    if (t.down && (t.x != lx || t.y != ly)) {
      Serial.printf("touch: %d,%d %s\n", t.x, t.y, t.justPressed ? "down" : "move");
      lx = t.x; ly = t.y;
    }
    if (!t.down) { lx = ly = -1; }
  }
#endif

  // ── 左右滑动切页（到头循环）──
  SwipeDir dir = hwSwipePoll();
  if (!s_inWifiConfig && s_pageCount > 1 && dir != SwipeDir::None) {
    if (dir == SwipeDir::Left) s_page = (s_page + 1) % s_pageCount;
    else                       s_page = (s_page + s_pageCount - 1) % s_pageCount;
    Serial.printf("swipe: -> page %d/%d\n", s_page + 1, s_pageCount);
  }

  // PWR 键：已连接时立刻刷一轮；自动连接等待中则直接进入 WiFi 配置
  if (hwBtnA().wasPressed && !s_inWifiConfig) {
    if (s_wifiConnected) {
      requestRefresh();
    } else if (s_wifiConnStart != 0) {
      s_wifiConnStart = 0;
      s_inWifiConfig = true;
      wfInit(s_wfCfg);
    }
  }

  // ── 分批刷新：每帧最多抓一个服务商 ──
  if (s_wifiConnected && !s_inWifiConfig) {
    if (!s_refreshActive && (s_lastRound == 0 || now - s_lastRound >= REFRESH_ROUND_MS)) {
      s_refreshActive = true;
      s_refreshIdx = 0;
      s_lastRound = now;
    }
    if (s_refreshActive && s_refreshIdx < s_pageCount) {
      fetchOne(s_pages[s_refreshIdx]);
      if (++s_refreshIdx >= s_pageCount) s_refreshActive = false;
    }
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
      requestRefresh();
      s_inWifiConfig = false;
    } else if (done) {
      s_inWifiConfig = false;
    }
    wfDraw(s_wfCfg);
  } else if (s_wifiConnected) {
    String ip = WiFi.localIP().toString();
    drawCurrentPage(ip.c_str(), nullptr);
  } else if (currentPageHasData()) {
    // WiFi 断开：不把界面跳走，继续显示缓存的用量，页头标出「网络断开 · N 分钟前」。
    // 只有从没取到过数据时才退回连接提示。
    drawCurrentPage("", "网络断开");
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
