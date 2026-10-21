#define spr (*hwCanvas())

#include "config.h"
#include "agent_client.h"
#include "deepseek_client.h"
#include "opencode_client.h"
#include "commandcode_client.h"
#include "usage_display.h"
#include "hw/hw.h"
#include "hw/net.h"
#include "hw/gesture.h"
#include "wifi_config.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <time.h>
#include <U8g2lib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

static const uint16_t MAIN_BG     = 0x0000;
static const uint16_t MAIN_YELLOW = 0xFFE0;

// 一轮刷新 = 把所有已配置的服务商抓一遍。抓取在后台任务里做，UI 循环完全不
// 参与：TLS 握手慢、接口超时都不会再卡住触摸和重绘。两家之间还留了
// USAGE_FETCH_GAP_MS，别让 core 0 被连续的 TLS 占满。
static const uint32_t REFRESH_ROUND_MS   = 60000;
static const uint32_t USAGE_FETCH_GAP_MS = 500;
static const uint32_t CONNECT_TIMEOUT_MS = 10000;  // 自动连接 10 秒超时

// Agent 状态页的节奏与上面那轮无关：用量一分钟一变，状态一秒一变。局域网里的
// 小请求只要几十毫秒；桥接不在时它最多阻塞 1.2 秒，但阻塞的是后台任务。
static const uint32_t AGENT_POLL_MS     = 1000;
static const uint32_t AGENT_POLL_MAX_MS = 30000;  // 桥接不在时最长退避到这个间隔
// 桥接在状态变化时直接推到这个端口（要跟 bridge.py 的 --udp-port 一致）
static const uint16_t AGENT_UDP_PORT    = 8788;

// 发现节奏：没找到时按 2/4/8/16/32 秒退避着喊，找到后每 60 秒确认一次
// （PC 换了 IP 也能跟上）。扫段的每帧发送量限制它别把一帧撑爆。
static const uint32_t AGENT_PROBE_MS       = 2000;
static const uint32_t AGENT_FOUND_PROBE_MS = 60000;
static const int      AGENT_SWEEP_PER_TICK = 16;
static const uint32_t AGENT_NOTFOUND_MS    = 60000;  // 找这么久还没人应答就改文案

static AppConfig        s_cfg;

// ── 后台任务与 UI 之间的数据 ──
// 两个网络任务在自己的变量里攒结果，换手时整块拷进 s_*Shared（持锁）；UI 每帧
// 只做一次非阻塞的"版本变了就拷到快照"，绘制读的一直是自己的快照。这样既不会
// 读到写了一半的结构体，也不会因为等锁卡住。
static SemaphoreHandle_t s_dataMux;
static OpenCodeUsage     s_ocShared;
static CommandCodeUsage  s_ccShared;
static DeepSeekUsage     s_dsShared;
static AgentStatus       s_agentShared;
static uint32_t          s_agentVer = 0;
static uint32_t          s_usageVer = 0;

static OpenCodeUsage     s_oc;          // UI 侧快照
static CommandCodeUsage  s_cc;
static DeepSeekUsage     s_ds;
static AgentStatus       s_agent;
static uint32_t          s_seenAgentVer = 0;
static uint32_t          s_seenUsageVer = 0;

// UI -> 后台任务
static volatile bool     s_cmdRefresh  = false;
static volatile bool     s_cmdAgentNow = false;
static volatile bool     s_cmdProbeNow = false;
static volatile uint32_t s_netGen      = 0;   // 每次连上 WiFi +1，任务据此重绑 UDP

// 页面顺序：Agent 状态在最前，其余三家与 TokenHub 一致（OpenCode Go →
// CommandCode → DeepSeek）。没配齐凭据的服务商不生成页面（只配了 DeepSeek
// 就仍是单页，滑动无效果）。
enum class Prov : uint8_t { Agent, Oc, Cc, Ds };
static Prov s_pages[4];
static int  s_pageCount = 0;
static int  s_page      = 0;

// ── Agent 发现/轮询状态（只在 agentNetTask 里读写）──
static WiFiUDP  s_agentUdp;
static bool     s_agentUdpReady = false;
static uint32_t s_lastAgentPoll = 0;  // 0 = 尚未轮询过（连上 WiFi 后立刻取一次）
static uint8_t  s_agentFailStreak = 0;

// 当前轮询目标。空 host = 还不知道桥接在哪，界面上显示"正在查找桥接"。
// 目标有两个来源：配置里的 agent_url（种子）与局域网发现的结果（后者更权威，
// 因为它是"此刻谁在应答"）。
static char     s_agentHost[48] = "";
static uint16_t s_agentPort     = 8787;
static char     s_agentPath[40] = "/agent";
static bool     s_agentFound    = false;  // 当前目标来自发现
static char     s_agentSaved[48] = "";    // 已写入 NVS 的地址（避免每次都写）
static uint16_t s_agentSavedPort = 0;

static WiFiUDP  s_agentDisc;
static bool     s_agentDiscReady = false;
static uint32_t s_lastProbe      = 0;
static uint8_t  s_agentProbeTries = 0;
static uint32_t s_agentSearchStart = 0;   // 0 = 已有目标
// 扫段的 /24：设备与 PC 常常不在同一网段（本机实测：PC 在 /22、板子在另一段），
// 广播跨不过去，但单播能凭路由到达。于是按上次那台 PC 的网段逐个问一遍——
// 换电脑后新 PC 大概率还在同一段。
static uint8_t  s_agentSweepNet[3] = {0, 0, 0};
static uint16_t s_agentSweepIdx    = 0;   // 0 = 未在扫；1..254 = 下一个候选

// 任务本地结果：抓取函数直接写这里，成功失败都保留上一次的有效数据
static AgentStatus       s_netAgent;
static OpenCodeUsage     s_netOc;
static CommandCodeUsage  s_netCc;
static DeepSeekUsage     s_netDs;

static volatile bool s_wifiConnected = false;
static volatile bool s_inWifiConfig  = false;
static uint32_t      s_wifiConnStart = 0;  // 自动连接开始时刻；0 = 未在自动连接
static WfConfig      s_wfCfg;

// UI 渲染节流状态
static uint32_t s_lastDrawMs      = 0;
static int      s_drawnPage       = -1;
static bool     s_drawnConnected  = false;

static void buildPages(const AppConfig& c) {
  s_pageCount = 0;
  // 状态页放第一页：它是唯一"此刻正在发生"的一页，开机默认停在它上面才有意义
  // （另外三页都是用量汇总，早一分钟晚一分钟看到没区别）。
  if (cfgAgentReady(c)) s_pages[s_pageCount++] = Prov::Agent;
  if (cfgOcReady(c)) s_pages[s_pageCount++] = Prov::Oc;
  if (cfgCcReady(c)) s_pages[s_pageCount++] = Prov::Cc;
  if (cfgDsReady(c)) s_pages[s_pageCount++] = Prov::Ds;
  if (s_page >= s_pageCount) s_page = 0;
}

// ── 数据换手 ──

static void publishAgent() {
  xSemaphoreTake(s_dataMux, portMAX_DELAY);
  s_agentShared = s_netAgent;
  s_agentVer++;
  xSemaphoreGive(s_dataMux);
}

static void publishUsage() {
  xSemaphoreTake(s_dataMux, portMAX_DELAY);
  s_ocShared = s_netOc;
  s_ccShared = s_netCc;
  s_dsShared = s_netDs;
  s_usageVer++;
  xSemaphoreGive(s_dataMux);
}

// UI 每帧调一次。返回"当前正在看的这一页的数据变了"——后台 Agent 每秒更新时
// 不该把正在看用量页的用户也拉去重绘。
static bool syncSharedData() {
  if (xSemaphoreTake(s_dataMux, 0) != pdTRUE) return false;

  bool agentChanged = false, usageChanged = false;
  if (s_agentVer != s_seenAgentVer) {
    s_agent = s_agentShared;
    s_seenAgentVer = s_agentVer;
    agentChanged = true;
  }
  if (s_usageVer != s_seenUsageVer) {
    s_oc = s_ocShared;
    s_cc = s_ccShared;
    s_ds = s_dsShared;
    s_seenUsageVer = s_usageVer;
    usageChanged = true;
  }
  xSemaphoreGive(s_dataMux);

  bool onAgent = (s_pageCount > 0 && s_pages[s_page] == Prov::Agent);
  return onAgent ? agentChanged : usageChanged;
}

// ── Agent 状态：目标地址与局域网发现 ──

// "a.b.c.d" -> 记住它的 /24，供扫段用
static void agentSetSweepFromHost() {
  unsigned a = 0, b = 0, c = 0, d = 0;
  if (!s_agentHost[0]) return;
  if (sscanf(s_agentHost, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
    s_agentSweepNet[0] = (uint8_t)a;
    s_agentSweepNet[1] = (uint8_t)b;
    s_agentSweepNet[2] = (uint8_t)c;
    (void)d;
  }
}

// 配置里的 agent_url 只当种子：先用它取一次，取不到就靠发现另找一台。
static void agentSeedFromConfig() {
  s_agentHost[0] = 0;
  s_agentFound = false;
  if (s_cfg.agent_url[0]) {
    if (agParseUrl(s_cfg.agent_url, s_agentHost, sizeof(s_agentHost),
                   s_agentPort, s_agentPath, sizeof(s_agentPath))) {
      agentSetSweepFromHost();
      Serial.printf("agent: seed from config %s:%u%s\n", s_agentHost, (unsigned)s_agentPort,
                    s_agentPath);
    } else {
      Serial.printf("agent: bad agent_url: %s\n", s_cfg.agent_url);
      s_agentHost[0] = 0;
      s_agentShared.lastError = agErrBadUrl;
    }
  }
  // 再压上"上次连上的那台"：重启后照着它直接连回去，不必等桥接的通告
  // （等通告的代价是屏幕上先写着 45 秒的"没找到桥接"）。
  char last[48];
  uint16_t lastPort = 0;
  if (agLoadLastHost(last, sizeof(last), lastPort)) {
    strlcpy(s_agentHost, last, sizeof(s_agentHost));
    s_agentPort = lastPort;
    Serial.printf("agent: resume last bridge %s:%u\n", s_agentHost, (unsigned)s_agentPort);
    if (!s_agentSweepNet[0] && !s_agentSweepNet[1] && !s_agentSweepNet[2]) {
      agentSetSweepFromHost();   // 配置没给种子时，拿上次的地址定扫段范围
    }
  }
}

// 绑定两个 UDP socket：8788 收桥接的即时推送，8789 收发发现报文。重连后要
// 重绑——接口状态变了，旧 socket 不再收包。只由 agentNetTask 调用。
static void startAgentNet() {
  s_agentUdpReady  = s_agentUdp.begin(AGENT_UDP_PORT) == 1;
  s_agentDiscReady = s_agentDisc.begin(AGENT_DISCOVERY_PORT) == 1;
  // 进入新网络：之前发现的地址大概率失效了，重新找；配置里的种子留着
  s_agentFound = false;
  s_agentProbeTries = 0;
  s_lastProbe = 0;
  s_agentSweepIdx = 0;
  s_lastAgentPoll = 0;
  Serial.printf("agent: udp push %s:%u, discovery %s:%u\n",
                s_agentUdpReady ? "on" : "FAILED", (unsigned)AGENT_UDP_PORT,
                s_agentDiscReady ? "on" : "FAILED", (unsigned)AGENT_DISCOVERY_PORT);
}

static void agentSendProbe(const IPAddress& dst) {
  s_agentDisc.beginPacket(dst, AGENT_DISCOVERY_PORT);
  s_agentDisc.write((const uint8_t*)AGENT_PROBE, strlen(AGENT_PROBE));
  s_agentDisc.endPacket();
}

// 广播问一遍：255.255.255.255 与所在子网的定向广播都发——有些网络只放行其中一种
static void agentProbeBroadcast() {
  agentSendProbe(IPAddress(255, 255, 255, 255));
  IPAddress ip = WiFi.localIP(), mask = WiFi.subnetMask();
  IPAddress bc((uint8_t)(ip[0] | ~mask[0]), (uint8_t)(ip[1] | ~mask[1]),
               (uint8_t)(ip[2] | ~mask[2]), (uint8_t)(ip[3] | ~mask[3]));
  if (!(bc == IPAddress(255, 255, 255, 255))) agentSendProbe(bc);
}

// 连不上桥接时重新开找：广播 + 扫一遍上次那台 PC 的 /24
static void agentRestartSearch() {
  s_agentFound = false;
  s_agentProbeTries = 0;
  s_lastProbe = 0;
  bool seeded = s_agentSweepNet[0] || s_agentSweepNet[1] || s_agentSweepNet[2];
  s_agentSweepIdx = seeded ? 1 : 0;
  Serial.printf("agent: bridge unreachable, searching%s\n",
                seeded ? " (broadcast + /24 sweep)" : " (broadcast)");
}

// 每轮跑一次：先收应答（源地址就是桥接 IP），再按节奏发问句或扫段
static void agentDiscoveryTick(uint32_t now) {
  if (!s_agentDiscReady) return;

  int n = s_agentDisc.parsePacket();
  if (n > 0) {
    char buf[64];
    int len = s_agentDisc.read(buf, sizeof(buf) - 1);
    uint16_t httpPort = 0;
    if (len > 0 && agParseReply(buf, len, httpPort)) {
      IPAddress from = s_agentDisc.remoteIP();
      char host[48];
      snprintf(host, sizeof(host), "%u.%u.%u.%u", from[0], from[1], from[2], from[3]);
      bool same = s_agentFound && strcmp(host, s_agentHost) == 0 && httpPort == s_agentPort;
      strlcpy(s_agentHost, host, sizeof(s_agentHost));
      s_agentPort = httpPort;
      s_agentSweepIdx = 0;       // 找到了，不必再扫
      if (!same) {
        agentSetSweepFromHost();
        s_agentFound = true;
        s_agentFailStreak = 0;
        s_lastAgentPoll = 0;     // 立刻拿这个地址取一次
        s_agentSearchStart = 0;
        Serial.printf("agent: bridge found at %s:%u\n", s_agentHost, (unsigned)s_agentPort);
      }
      return;
    }
  }

  // 扫段：按上次那台 PC 的 /24 逐个单播（广播跨不过网段，单播可以）
  if (s_agentSweepIdx >= 1 && s_agentSweepIdx <= 254) {
    for (int i = 0; i < AGENT_SWEEP_PER_TICK && s_agentSweepIdx <= 254;
         i++, s_agentSweepIdx++) {
      agentSendProbe(IPAddress(s_agentSweepNet[0], s_agentSweepNet[1], s_agentSweepNet[2],
                               (uint8_t)s_agentSweepIdx));
    }
    return;
  }

  uint32_t shift = s_agentProbeTries > 4 ? 4 : s_agentProbeTries;
  uint32_t interval = s_agentFound ? AGENT_FOUND_PROBE_MS : (AGENT_PROBE_MS << shift);
  if (s_lastProbe != 0 && now - s_lastProbe < interval) return;
  s_lastProbe = now;
  if (!s_agentFound && s_agentProbeTries < 8) s_agentProbeTries++;
  agentProbeBroadcast();
}

static void startNtp() {
  configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
}

// ── 后台任务 ──

// Agent 状态：发现 + 每秒轮询 + 收 UDP 推送。所有阻塞（最坏 1.2 秒的 TCP 超时）
// 都只发生在 core 0 的这个任务里。
static void agentNetTask(void*) {
  uint32_t lastGen = 0;

  for (;;) {
    if (!s_wifiConnected || s_inWifiConfig) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (lastGen != s_netGen) {
      lastGen = s_netGen;
      startAgentNet();
    }

    uint32_t now = millis();
    if (s_cmdProbeNow) { s_cmdProbeNow = false; s_lastProbe = 0; }
    if (s_cmdAgentNow) { s_cmdAgentNow = false; s_lastAgentPoll = 0; }

    agentDiscoveryTick(now);

    bool dirty = false;

    if (s_agentHost[0]) {
      uint32_t backoff  = AGENT_POLL_MS << s_agentFailStreak;
      uint32_t interval = backoff > AGENT_POLL_MAX_MS ? AGENT_POLL_MAX_MS : backoff;
      if (s_lastAgentPoll == 0 || now - s_lastAgentPoll >= interval) {
        s_lastAgentPoll = now;
        if (agFetchHost(s_netAgent, s_agentHost, s_agentPort, s_agentPath)) {
          s_agentFailStreak = 0;
          // 连上了就记下来：下次重启直接照这个地址连（写 NVS 只在变化时发生）
          if (strcmp(s_agentHost, s_agentSaved) != 0 || s_agentPort != s_agentSavedPort) {
            agSaveLastHost(s_agentHost, s_agentPort);
            strlcpy(s_agentSaved, s_agentHost, sizeof(s_agentSaved));
            s_agentSavedPort = s_agentPort;
          }
        } else {
          if (s_agentFailStreak < 6) s_agentFailStreak++;
          // 连不上就把发现重开一遍：PC 换了 IP、或者换了另一台机器
          if (s_agentFailStreak == 5) agentRestartSearch();
        }
        dirty = true;
      }
    } else {
      // 还没有目标地址：界面上说清楚是"正在找"还是"没找到"
      if (s_agentSearchStart == 0) s_agentSearchStart = now;
      uint32_t err = (now - s_agentSearchStart > AGENT_NOTFOUND_MS)
                       ? agErrNotFound : agErrSearching;
      if (s_netAgent.lastError != err) {
        s_netAgent.lastError = err;
        dirty = true;
      }
    }

    // 桥接在状态变化时直接推一份过来，比轮询快得多。parsePacket 不阻塞，
    // 没有包时立刻返回 0。只在状态真的变了时打日志——一次状态变化一行，
    // 既能确认这条最快的链路通不通，也不会变成每秒刷屏。
    if (s_agentUdpReady) {
      int n = s_agentUdp.parsePacket();
      if (n > 0) {
        char buf[384];
        int len = s_agentUdp.read(buf, sizeof(buf) - 1);
        if (len > 0) {
          buf[len] = 0;
          bool ok = agApply(s_netAgent, buf, true);
          dirty = true;
          if (ok) {
            s_agentFailStreak = 0;
            // 打的是卡片上真正显示的内容（第三张卡片：执行中显示动作，其余显示
            // 项目名），所以在终端里也能"看屏"，不用凑到板子跟前。
            if (s_netAgent.tool[0]) Serial.printf("agent: push %s %s", s_netAgent.state, s_netAgent.tool);
            else                    Serial.printf("agent: push %s", s_netAgent.state);
            Serial.printf(" | %s\n", s_netAgent.action[0] ? s_netAgent.action : s_netAgent.project);
          }
        }
      }
    }

    if (dirty) publishAgent();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// 用量：一轮 60 秒，每次只抓一家，抓完歇一下。TLS 握手/超时都阻塞在这里，
// UI 不受影响。
static void usageNetTask(void*) {
  uint32_t lastRound = 0;
  int      idx       = 0;
  bool     active    = false;

  for (;;) {
    if (!s_wifiConnected || s_inWifiConfig) {
      active = false;
      lastRound = 0;
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    if (s_cmdRefresh) {
      s_cmdRefresh = false;
      active = false;
      lastRound = 0;
    }

    uint32_t now = millis();
    if (!active && (lastRound == 0 || now - lastRound >= REFRESH_ROUND_MS)) {
      active = true;
      idx = 0;
      lastRound = now;
    }
    if (!active) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    if (idx >= s_pageCount) {
      active = false;
      continue;
    }

    switch (s_pages[idx++]) {
      case Prov::Oc: ocFetch(s_netOc, s_cfg.oc_workspace_id, s_cfg.oc_cookie, s_cfg.oc_api_key); break;
      case Prov::Cc: ccFetch(s_netCc, s_cfg.cc_cookie); break;
      case Prov::Ds: dsFetchToday(s_netDs, s_cfg.ds_token); break;
      // 状态页不进 60 秒轮次：它按秒更新（见 agentNetTask），塞进轮次里只会
      // 让状态最多滞后一分钟。
      case Prov::Agent: break;
    }
    publishUsage();
    vTaskDelay(pdMS_TO_TICKS(USAGE_FETCH_GAP_MS));
  }
}

// ── 绘制 ──

// 画当前页。netStatus 非空时页头改用它作为状态文案（WiFi 断开时传「网络断开」），
// 优先级高于数据自带的 lastError。
static void drawCurrentPage(const char* ip, const char* netStatus) {
  if (s_pageCount == 0) {
    usageDisplayDrawEmpty(ip);
    return;
  }
  switch (s_pages[s_page]) {
    case Prov::Agent: usageDisplayDrawAgent(s_agent, ip, s_page, s_pageCount, netStatus); break;
    case Prov::Oc: usageDisplayDrawOc(s_oc, ip, s_page, s_pageCount, netStatus); break;
    case Prov::Cc: usageDisplayDrawCc(s_cc, ip, s_page, s_pageCount, netStatus); break;
    case Prov::Ds: usageDisplayDrawDs(s_ds, ip, s_page, s_pageCount, netStatus); break;
  }
}

// 当前页是否已有可显示的数据——决定断网时是继续显示缓存还是给连接提示
static bool currentPageHasData() {
  if (s_pageCount == 0) return false;
  switch (s_pages[s_page]) {
    case Prov::Agent: return s_agent.valid;
    case Prov::Oc: return s_oc.valid;
    case Prov::Cc: return s_cc.valid;
    case Prov::Ds: return s_ds.valid;
  }
  return false;
}

void setup() {
  hwInit();

  spr.fillScreen(MAIN_BG);
  spr.setFont(u8g2_font_wqy16_t_gb2312b);
  spr.setTextColor(MAIN_YELLOW);
  spr.setCursor(SAFE_L, SAFE_T + 60);
  spr.print("Booting...");
  hwDisplayPush();

  s_dataMux = xSemaphoreCreateMutex();
  loadConfig(s_cfg);
  buildPages(s_cfg);
  agentSeedFromConfig();
  Serial.printf("boot: %d page(s) ready (agent=%d oc=%d cc=%d ds=%d)\n",
                s_pageCount, (int)cfgAgentReady(s_cfg), (int)cfgOcReady(s_cfg),
                (int)cfgCcReady(s_cfg), (int)cfgDsReady(s_cfg));

  // 网络请求全部放在 core 0 的后台任务里；loop() 只负责触摸和绘制。
  if (xTaskCreatePinnedToCore(agentNetTask, "agent_net", 8192, nullptr, 1, nullptr, 0) != pdPASS)
    Serial.println("boot: agent task create failed");
  if (xTaskCreatePinnedToCore(usageNetTask, "usage_net", 12288, nullptr, 1, nullptr, 0) != pdPASS)
    Serial.println("boot: usage task create failed");

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

  // 自动旋转。放在触摸之后、手势之前：方向一变，手势层会作废正在进行的滑动。
  // MADCTL 改向后面板会立刻把现有帧转过来，但主界面是按需重绘的，这里强制
  // 重画一次，顺带刷新时钟/状态文案。WiFi 配置页每帧都重画，不需要额外处理。
  if (hwRotationPoll()) {
    s_drawnPage  = -1;
    s_lastDrawMs = 0;
  }

  if (!s_wifiConnected) {
    if (WiFi.status() == WL_CONNECTED) {
      s_wifiConnected = true;
      s_wifiConnStart = 0;
      startNtp();
      s_netGen++;
      s_cmdRefresh = true;       // 连上就立刻刷一轮用量
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

  // PWR 键：已连接时立刻刷一轮（顺带把状态页也立刻重试，不用等退避）；
  // 自动连接等待中则直接进入 WiFi 配置
  if (hwBtnA().wasPressed && !s_inWifiConfig) {
    if (s_wifiConnected) {
      s_cmdRefresh  = true;
      s_cmdAgentNow = true;
      s_cmdProbeNow = true;
    } else if (s_wifiConnStart != 0) {
      s_wifiConnStart = 0;
      s_inWifiConfig = true;
      wfInit(s_wfCfg);
    }
  }

  // BOOT 键进 WiFi 配置
  if (hwBtnBoot().wasPressed && !s_inWifiConfig) {
    s_inWifiConfig = true;
    wfInit(s_wfCfg);
  }

  bool dataChanged = syncSharedData();

  if (s_inWifiConfig) {
    bool done = wfTick(s_wfCfg);
    if (s_wfCfg.state == WfState::Connected) {
      s_wifiConnected = true;
      startNtp();
      s_netGen++;
      s_cmdRefresh = true;
      s_inWifiConfig = false;
      s_drawnPage = -1;          // 退出配置界面后强制重画主界面
    } else if (done) {
      s_inWifiConfig = false;
      s_drawnPage = -1;
    }
    wfDraw(s_wfCfg);
    hwDisplayPush();
    delay(16);
    return;
  }

  // ── 按需重绘 ──
  // 以前每帧重绘 + 每帧推 480×480，就算没有网络请求也把主循环占满了。现在只有
  // 内容真的变了才画：翻页/数据更新/网络状态变化立即画，其余按 1 秒（状态页）
  // 或 10 秒（用量页，只有底部时钟和"多久前"会变）兜底刷新一次。
  bool periodDue = (s_pageCount > 0 && s_pages[s_page] == Prov::Agent)
                     ? (now - s_lastDrawMs >= 1000)
                     : (now - s_lastDrawMs >= 10000);
  bool pageChanged = (s_drawnPage != s_page);
  bool connChanged = (s_drawnConnected != s_wifiConnected);

  if (pageChanged || connChanged || dataChanged || periodDue || s_lastDrawMs == 0) {
    if (s_wifiConnected) {
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
    s_lastDrawMs = now;
    s_drawnPage = s_page;
    s_drawnConnected = s_wifiConnected;
  }

  delay(5);
}
