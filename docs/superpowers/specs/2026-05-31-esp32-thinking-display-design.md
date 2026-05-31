# ESP32 OpenCode 思考状态显示 — 设计文档

> 日期: 2026-05-31  
> 目标硬件: Waveshare ESP32-S3-Touch-AMOLED-2.16 (480×480, Canvas 184×224)

## 1. 概述

当前 ESP32 固件已支持通过 HTTP `?state=running/done/failed` 切换"思考中"画面，但存在以下不足：
- 思考过程中无进度反馈（无步骤计数、无耗时、无动画变化）
- 思考完成后直接跳回用量页面，无"思考完成"确认提示
- 失败场景无差异化显示

本设计引入两个变更：
1. **OpenCode 插件**：hook 会话事件，通过 HTTP 向 ESP32 推送实时状态
2. **ESP32 固件增强**：扩展 HTTP 协议，实现动画思考画面、完成/失败提示

## 2. 架构

```
OpenCode (PC, 运行 opencode 或插件)       ESP32 (硬件, 192.168.31.243)
┌──────────────────────────────┐   HTTP  ┌───────────────────────────┐
│ .opencode/plugins/           │  ─────→ │ WiFiServer s_server(80)   │
│   esp32-status.ts            │         │     ↓                     │
│                              │         │ handleHttpStatus()        │
│ Hooks:                       │         │     ↓                     │
│  session.status → running    │         │ s_thinking, s_progress    │
│  tool.execute.after → step++ │         │     ↓                     │
│  session.idle → done         │         │ usageDisplayDrawThinking()│
│  session.error → failed      │         │ usageDisplayDrawDone()    │
│                              │         │ usageDisplayDrawFailed()  │
│ Communication:               │         │     ↓                     │
│  $`curl ...?state=...`      │         │ 480×480 AMOLED 显示       │
└──────────────────────────────┘         └───────────────────────────┘
```

## 3. ESP32 HTTP 协议

### 3.1 请求格式

| 参数 | 类型 | 说明 |
|------|------|------|
| `state` | string | `running` / `done` / `failed` |
| `step` | int | 当前步骤计数 (仅 running 状态有意义) |
| `total` | int | 估计总步骤数 (可选, 用于百分比计算) |
| `msg` | string | 当前操作描述 (可选, UTF-8 URL 编码) |

### 3.2 请求示例

```
GET /?state=running                        → 开始思考
GET /?state=running&step=1&msg=读取文件     → 第1步
GET /?state=running&step=2&msg=搜索代码     → 第2步
GET /?state=running&step=3&msg=编辑文件     → 第3步
GET /?state=done                           → 思考完成
GET /?state=failed                         → 任务失败
```

### 3.3 状态机

```
                  state=running
    [用量/时间页面] ───────────→ [思考中画面]
         ↑                          │
         │              state=running&step=N
         │              (更新步骤/描述)
         │                          │
         │              state=done   │  state=failed
         │    ┌──────────┘           └──────────┐
         │    ↓                                  ↓
         │ [思考完成画面 2s]              [任务失败画面 2s]
         │    ↓                                  ↓
         └────┴──────────────────────────────────┘
              (自动返回)
```

## 4. ESP32 固件改动

### 4.1 新增全局变量 (`main.cpp`)

```cpp
static int s_thinkingStep = 0;       // 当前步骤数
static char s_thinkingMsg[64];       // 当前操作描述
static int s_totalSteps = 0;         // 估计总步骤 (可选)
static uint32_t s_doneSince = 0;     // done/failed 画面开始时间
static bool s_showDone = false;      // 是否显示完成画面
static bool s_showFailed = false;    // 是否显示失败画面
```

### 4.2 HTTP 解析增强 (`handleHttpStatus`)

在现有 `state=running/done/failed` 基础上，额外解析：
- `step=N` — 解析为 `s_thinkingStep`
- `msg=...` — 解析为 `s_thinkingMsg`，需 URL decode

### 4.3 主循环 `loop()` 修改

新增两个状态优先级（在彩蛋之后）：
```cpp
if (s_showDone) {
    usageDisplayDrawDone(s_thinkingStep, s_doneSince);
    if (millis() - s_doneSince >= 2000) s_showDone = false;
} else if (s_showFailed) {
    usageDisplayDrawFailed(s_doneSince);
    if (millis() - s_doneSince >= 2000) s_showFailed = false;
} else if (s_thinking) {
    usageDisplayDrawThinking(now - s_thinkingSince, s_thinkingStep, s_thinkingMsg);
}
```

`state=done` 触发时设置 `s_showDone=true, s_thinking=false, s_doneSince=now`。  
`state=failed` 同理设置 `s_showFailed`。

### 4.4 显示函数改动 (`usage_display.cpp`)

#### 4.4.1 `usageDisplayDrawThinking()` 签名变更

```cpp
void usageDisplayDrawThinking(uint32_t elapsedMs, int step, const char* msg)
```

新增内容：
- **不定长滚动进度条**：1秒全周期，光条从左滚到右再循环
- **步骤计数**：`步骤 N` 文本（14px 字体，白色）
- **操作描述**：如果有 `msg`，显示在进度条下方（12px 字体，暗灰色）
- **实时计时**：`已用: Xs` (14px 字体，暗灰色)

#### 4.4.2 新增: `usageDisplayDrawDone()`

```cpp
void usageDisplayDrawDone(int steps, uint32_t doneSince)
```

- 绿色大号对勾 ✓（用 U8g2 字符 `✓` 或绘制符号）
- `思考完成` 绿色大字
- 统计行：`X 步 · 用时 Xs`
- 底部：`Xs 后返回...` 倒计时

#### 4.4.3 新增: `usageDisplayDrawFailed()`

```cpp
void usageDisplayDrawFailed(uint32_t doneSince)
```

- 红色大号叉号 ✗
- `任务失败` 红色大字
- 底部：`Xs 后返回...` 倒计时

### 4.5 不定长进度条实现

```cpp
static void drawIndeterminateBar(int x, int y, int w, uint32_t elapsedMs) {
    // 光条宽度 = 总宽度的 30%
    int barW = w * 30 / 100;
    // 1秒周期: 光条从左到右
    uint32_t cycle = elapsedMs % 1000;
    int pos = (cycle * (w + barW) / 1000) - barW;  // -barW 到 w
    // 背景
    spr.fillRect(x, y, w, 4, COL_DIM);
    // 光条（限制在范围内）
    int drawX = constrain(pos, 0, w - barW);
    spr.fillRect(x + drawX, y, barW, 4, COL_GREEN);
}
```

## 5. OpenCode 插件 (`esp32-status.ts`)

### 5.1 位置

`.opencode/plugins/esp32-status.ts`

### 5.2 逻辑

```typescript
import type { Plugin } from "@opencode-ai/plugin"

const ESP32_URL = "http://192.168.31.243/";
let stepCount = 0;
let isRunning = false;

export const Esp32StatusPlugin: Plugin = async ({ $ }) => {
  return {
    // 监听所有事件
    event: async ({ event }) => {
      switch (event.type) {
        case "session.status":
          if (event.properties.status !== "idle" && !isRunning) {
            isRunning = true;
            stepCount = 0;
            await $`curl -s "${ESP32_URL}?state=running"`;
          }
          break;

        case "tool.execute.after":
          if (isRunning) {
            stepCount++;
            const toolName = encodeURIComponent(event.properties.tool || "");
            await $`curl -s "${ESP32_URL}?state=running&step=${stepCount}&msg=${toolName}"`;
          }
          break;

        case "session.idle":
          if (isRunning) {
            isRunning = false;
            const elapsed = event.properties.elapsed || 0;
            await $`curl -s "${ESP32_URL}?state=done&steps=${stepCount}&msg=${elapsed}ms"`;
            stepCount = 0;
          }
          break;

        case "session.error":
          if (isRunning) {
            isRunning = false;
            await $`curl -s "${ESP32_URL}?state=failed"`;
            stepCount = 0;
          }
          break;
      }
    },
  };
};
```

### 5.3 注意事项

- 插件在 Windows 上需确保 `curl.exe` 可用（系统自带）
- `message.part.updated` 事件也可用于更细粒度进度，但会增加 HTTP 请求频率，暂不启用
- 插件不阻塞会话：curl 是 fire-and-forget（`-s` 静默，超时短）

## 6. 文件变更清单

| 文件 | 变更类型 | 说明 |
|------|---------|------|
| `.opencode/plugins/esp32-status.ts` | 新增 | OpenCode 插件 |
| `src/main.cpp` | 修改 | 新增状态变量 + HTTP 解析 + loop 逻辑 |
| `src/usage_display.h` | 修改 | 新增函数声明 |
| `src/usage_display.cpp` | 修改 | 增强 thinking 画面 + 新增 done/failed 画面 |

## 7. 边界与限制

- **无百分比**：OpenCode 不暴露总步骤数，使用不定长进度条 + 步骤计数代替
- **步骤定义**：一次 `tool.execute.after` = 一个步骤（工具调用完成时计数）
- **HTTP 延迟**：WiFi 网络下 curl 往返约 10-50ms，不影响 ESP32 60FPS 渲染
- **并发安全**：ESP32 单线程处理 HTTP，无竞争问题
- **NFC URL 编码**：`msg` 参数中的中文/特殊字符需要 `encodeURIComponent`
- **插件仅在 OpenCode 启动时加载**，如果用户在 OpenCode 运行中修改插件，需要重启

## 8. 测试验证

1. **插件端**：启动 OpenCode，发送任意消息，观察 ESP32 屏幕是否正确切换状态
2. **ESP32 端**：用浏览器直接访问 `http://192.168.31.243/?state=running` 等 URL，验证画面切换
3. **异常场景**：OpenCode 关闭后 ESP32 无 HTTP 请求，s_thinking 保持（用户手动 PWR 取消）
4. **网络断开**：curl 静默失败，ESP32 保持当前状态，不影响 OpenCode 主流程
