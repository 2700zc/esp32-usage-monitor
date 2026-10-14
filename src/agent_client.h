#pragma once
#include <Arduino.h>

// ZCode 的实时状态。状态由 PC 上的桥接进程维护（tools/agent-bridge/bridge.py）：
// 固件每秒轮询一次，桥接从轮询请求的源地址学到设备位置后，还会在状态变化的
// 瞬间直接 UDP 推一份过来。两条路送的是同一份 JSON，所以解析只有一份。
enum AgentErr : uint8_t {
  agErrOk = 0,
  agErrBadUrl,     // agent_url 不是 http://host[:port][/path] 形式
  agErrNetwork,    // 连不上：PC 关机、IP 变了、防火墙拦了
  agErrHttp,       // 桥接回了非 200
  agErrParse,
  agErrSearching,  // 还没有目标地址，正在局域网里找桥接
  agErrNotFound,   // 找了一分钟都没人应答
};

// ── 局域网发现 ──
// 设备不该知道桥接在哪台机器上：换电脑、DHCP 换 IP 都不该重刷固件。于是它喊一
// 句问话（广播，或按上次记住的网段逐个单播），谁在跑桥接谁应答；应答带 HTTP
// 端口，IP 从报文源地址取。两个常量必须与 bridge.py 的 --discovery-port 一致。
static const uint16_t AGENT_DISCOVERY_PORT = 8789;
extern const char     AGENT_PROBE[];

// 解析发现应答，取出 HTTP 端口。只认整串前缀，免得把状态推送的 JSON 误判成应答。
bool agParseReply(const char* buf, int len, uint16_t& httpPort);

struct AgentStatus {
  bool     valid;           // 至少成功取到过一次
  uint32_t fetchedAt;       // 最后一次成功更新的 millis
  uint32_t lastError;       // AgentErr
  int      lastHttp;
  char     lastDetail[40];

  char     state[12];       // idle | thinking | tool | waiting | done
  char     tool[24];        // state == tool 时的工具名
  char     action[64];      // 正在做的事：Bash 的命令、Edit 的文件名等
  char     project[32];     // 会话所属目录名
  uint32_t ageSec;          // 距桥接收到的最后一个事件有多久
  uint8_t  activeSessions;
  bool     stale;           // 桥接认为这个状态可能已经过期
  bool     pushed;          // 最后一次更新来自 UDP 推送（便于排查链路）
};

// 把 agent_url 拆成 host / port / path。设备端不解析域名——桥接在局域网里，
// 地址要么是 IP，要么是错的，报错比悄悄连一个别的东西好。
bool agParseUrl(const char* url, char* host, size_t hostSz,
                uint16_t& port, char* path, size_t pathSz);

// HTTP 轮询。host 是点分十进制，path 形如 "/agent"。
bool agFetchHost(AgentStatus& out, const char* host, uint16_t port, const char* path);

// 解析一份状态 JSON（轮询与 UDP 推送共用）。pushed 只影响 out.pushed 标记。
bool agApply(AgentStatus& out, const char* json, bool pushed);
