#pragma once
#include <Arduino.h>

// ZCode 的实时状态。状态由 PC 上的桥接进程维护（tools/agent-bridge/bridge.py）：
// 固件每秒轮询一次，桥接从轮询请求的源地址学到设备位置后，还会在状态变化的
// 瞬间直接 UDP 推一份过来。两条路送的是同一份 JSON，所以解析只有一份。
enum AgentErr : uint8_t {
  agErrOk = 0,
  agErrNoUrl,     // config 里没填 agent_url
  agErrBadUrl,    // agent_url 不是 http://host[:port][/path] 形式
  agErrNetwork,   // 连不上桥接：PC 关机、IP 变了、防火墙拦了
  agErrHttp,      // 桥接回了非 200
  agErrParse,
};

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

// HTTP 轮询。url 形如 http://192.168.1.10:8787/agent。
bool agFetch(AgentStatus& out, const char* url);

// 解析一份状态 JSON（轮询与 UDP 推送共用）。pushed 只影响 out.pushed 标记。
bool agApply(AgentStatus& out, const char* json, bool pushed);
