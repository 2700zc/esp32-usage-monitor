# ESP32 OpenCode 思考状态显示 — 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** ESP32 实时显示 OpenCode 思考进度（不定长光条 + 步骤计数 + 计时），思考完成后显示完成/失败画面。

**Architecture:** OpenCode 插件通过 hook 系统捕获 session.status / tool.execute.after / session.idle / session.error 事件，用 curl 向 ESP32 HTTP 服务器推送状态；ESP32 端扩展 HTTP 解析逻辑，增强思考画面（不定长进度条、步骤计数、实时计时），新增完成/失败过渡画面。

**Tech Stack:** TypeScript (OpenCode 插件), C++/Arduino (ESP32 固件), U8g2 字体, Arduino_GFX Canvas

---

## 文件结构

| 文件 | 职责 | 变更 |
|------|------|------|
| `.opencode/plugins/esp32-status.ts` | 监听 OpenCode 事件，向 ESP32 推送 HTTP 状态 | **新增** |
| `src/usage_display.h` | 声明绘制函数接口 | 修改（签名变更+新增声明） |
| `src/usage_display.cpp` | 思考/完成/失败画面绘制实现 | 修改（核心改动） |
| `src/main.cpp` | 状态变量、HTTP 解析、主循环调度 | 修改（解析+状态机） |

---

### Task 1: 创建 OpenCode 插件

**Files:**
- Create: `D:\esp32\.opencode\plugins\esp32-status.ts`

- [ ] **Step 1: 创建插件文件**

```typescript
import type { Plugin } from "@opencode-ai/plugin"

const ESP32_URL = "http://192.168.31.243/";
let stepCount = 0;
let isRunning = false;

export const Esp32StatusPlugin: Plugin = async ({ $ }) => {
  return {
    event: async ({ event }) => {
      if (event.type === "session.status") {
        const status = (event.properties as any)?.status;
        if (status && status !== "idle" && !isRunning) {
          isRunning = true;
          stepCount = 0;
          await $`curl -s "${ESP32_URL}?state=running"`.quiet();
        }
      }
      if (event.type === "session.idle") {
        if (isRunning) {
          isRunning = false;
          await $`curl -s "${ESP32_URL}?state=done&steps=${stepCount}"`.quiet();
          stepCount = 0;
        }
      }
      if (event.type === "session.error") {
        if (isRunning) {
          isRunning = false;
          await $`curl -s "${ESP32_URL}?state=failed"`.quiet();
          stepCount = 0;
        }
      }
    },

    "tool.execute.after": async (input: { tool?: string }) => {
      if (isRunning && input.tool) {
        stepCount++;
        const encoded = encodeURIComponent(input.tool);
        await $`curl -s "${ESP32_URL}?state=running&step=${stepCount}&msg=${encoded}"`.quiet();
      }
    },
  };
};
```

- [ ] **Step 2: 确认插件目录存在**

```powershell
if (!(Test-Path -LiteralPath ".opencode\plugins")) {
    New-Item -ItemType Directory -Path ".opencode\plugins" -Force
}
```

- [ ] **Step 3: 编译验证**

OpenCode 在启动时会自动加载 `.opencode/plugins/` 下的 TypeScript 文件。确保语法无错误即可（OpenCode 内部用 Bun 编译）。

- [ ] **Step 4: 提交**

```bash
git add .opencode/plugins/esp32-status.ts
git commit -m "feat: add OpenCode plugin for ESP32 thinking status"
```

---

### Task 2: 更新 usage_display.h 声明

**Files:**
- Modify: `D:\esp32\esp32-usage-monitor\src\usage_display.h`

- [ ] **Step 1: 修改函数签名，新增声明**

将 `usage_display.h` 内容替换为：

```cpp
#pragma once
#include "api_client.h"

void usageDisplayDraw(const UsageData& data, const char* ip = nullptr);
void usageDisplayDrawTime(const char* ip, bool timeValid = false);
void usageDisplayDrawThinking(uint32_t elapsedMs, int step, const char* msg);
void usageDisplayDrawDone(int steps, uint32_t elapsedMs);
void usageDisplayDrawFailed(uint32_t elapsedMs);
void usageDisplayDrawEaster555(uint32_t elapsedMs);
```

关键变更：
- `usageDisplayDrawThinking` 新增 `step` 和 `msg` 参数
- 新增 `usageDisplayDrawDone` — 思考完成画面
- 新增 `usageDisplayDrawFailed` — 任务失败画面

- [ ] **Step 2: 提交**

```bash
git add src/usage_display.h
git commit -m "feat: add done/failed display declarations, update thinking signature"
```

---

### Task 3: 实现新的绘制函数 (usage_display.cpp)

**Files:**
- Modify: `D:\esp32\esp32-usage-monitor\src\usage_display.cpp`

此任务分 4 个子步骤：辅助函数 → thinking 增强 → done 画面 → failed 画面。

- [ ] **Step 1: 添加不定长进度条辅助函数**

在文件头部 `accentColor()` 函数之后、`formatTime()` 之前插入：

```cpp
static void drawIndeterminateBar(int x, int y, int w, uint32_t elapsedMs) {
  int barW = w * 30 / 100;
  uint32_t cycle = elapsedMs % 1000;
  int pos = (int)(((int64_t)cycle * (w + barW)) / 1000) - barW;
  int drawX = pos;
  if (drawX < 0) drawX = 0;
  if (drawX > w - barW) drawX = w - barW;
  spr.fillRect(x, y, w, 4, COL_DIM);
  spr.fillRect(x + drawX, y, barW, 4, COL_GREEN);
}
```

- [ ] **Step 2: 重写 `usageDisplayDrawThinking()`**

将第 190-203 行的现有实现替换为：

```cpp
void usageDisplayDrawThinking(uint32_t elapsedMs, int step, const char* msg) {
  if (!loadImg555()) { spr.fillScreen(COL_BG); return; }
  spr.fillScreen(COL_BG);

  // 555 Logo 闪烁 (200ms 周期, 50% 占空比)
  uint32_t logoCycle = elapsedMs % 200;
  if (logoCycle < 100) {
    int cx = SAFE_L + SAFE_W / 2;
    spr.draw16bitRGBBitmap(cx - s_img555W / 2, SAFE_T + 24,
                           s_img555, s_img555W, s_img555H);
  }

  // "思考中" 文字 + 省略号动画 (600ms 周期: 3点、4点、5点)
  spr.setFont(u8g2_font_wqy16_t_gb2312b);
  spr.setTextColor(COL_TEXT);
  spr.setCursor(SAFE_L + 20, SAFE_T + 90);
  uint32_t dotCycle = (elapsedMs % 600) / 200;
  spr.print("思考中");
  for (uint32_t i = 0; i <= dotCycle; i++) spr.print(".");

  // 不定长滚动进度条
  drawIndeterminateBar(SAFE_L + 8, SAFE_T + 108, SAFE_W - 16, elapsedMs);

  // 步骤计数
  spr.setFont(u8g2_font_wqy14_t_gb2312b);
  spr.setTextColor(COL_TEXT);
  spr.setCursor(SAFE_L + 8, SAFE_T + 130);
  spr.printf("步骤 %d", step);

  // 操作描述 (如果有)
  if (msg && msg[0]) {
    spr.setFont(u8g2_font_wqy12_t_gb2312b);
    spr.setTextColor(COL_DIM);
    spr.setCursor(SAFE_L + 8, SAFE_T + 150);
    // 截断过长的描述
    char truncated[32];
    snprintf(truncated, sizeof(truncated), "%.28s", msg);
    spr.print(truncated);
  }

  // 实时计时
  spr.setFont(u8g2_font_wqy14_t_gb2312b);
  spr.setTextColor(COL_DIM);
  spr.setCursor(SAFE_L + 8, SAFE_T + 172);
  uint32_t sec = elapsedMs / 1000;
  if (sec < 60) {
    spr.printf("已用: %us", (unsigned int)sec);
  } else if (sec < 3600) {
    spr.printf("已用: %um%us", (unsigned int)(sec / 60), (unsigned int)(sec % 60));
  } else {
    spr.printf("已用: %uh%um", (unsigned int)(sec / 3600), (unsigned int)((sec % 3600) / 60));
  }
}
```

- [ ] **Step 3: 新增 `usageDisplayDrawDone()`**

在文件末尾（`usageDisplayDrawEaster555` 之后）追加：

```cpp
void usageDisplayDrawDone(int steps, uint32_t elapsedMs) {
  spr.fillScreen(COL_BG);

  // 大号绿色对勾 (28px 字体)
  spr.setFont(u8g2_font_wqy16_t_gb2312b);
  spr.setTextColor(COL_GREEN);
  int cx = SAFE_L + SAFE_W / 2;
  spr.setCursor(cx - 12, SAFE_T + 60);
  spr.print("OK");

  // "思考完成" 大字
  spr.setCursor(cx - 40, SAFE_T + 100);
  spr.print("思考完成");

  // 统计信息
  spr.setFont(u8g2_font_wqy14_t_gb2312b);
  spr.setTextColor(COL_DIM);
  spr.setCursor(cx - 50, SAFE_T + 135);
  uint32_t sec = elapsedMs / 1000;
  if (sec < 60) {
    spr.printf("%d 步 · 用时 %us", steps, (unsigned int)sec);
  } else if (sec < 3600) {
    spr.printf("%d 步 · 用时 %um%us", steps, (unsigned int)(sec / 60), (unsigned int)(sec % 60));
  } else {
    spr.printf("%d 步 · 用时 %uh%um", steps, (unsigned int)(sec / 3600), (unsigned int)((sec % 3600) / 60));
  }

  // 倒计时提示
  spr.setCursor(cx - 40, SAFE_T + 165);
  spr.printf("%us 后返回...", (unsigned int)(2 - elapsedMs / 1000));
}
```

- [ ] **Step 4: 新增 `usageDisplayDrawFailed()`**

在 `usageDisplayDrawDone` 之后追加：

```cpp
void usageDisplayDrawFailed(uint32_t elapsedMs) {
  spr.fillScreen(COL_BG);

  // 红色叉号 (28px)
  spr.setFont(u8g2_font_wqy16_t_gb2312b);
  spr.setTextColor(COL_RED);
  int cx = SAFE_L + SAFE_W / 2;
  spr.setCursor(cx - 12, SAFE_T + 80);
  spr.print("X");

  // "任务失败" 大字
  spr.setCursor(cx - 40, SAFE_T + 120);
  spr.print("任务失败");

  // 倒计时提示
  spr.setFont(u8g2_font_wqy14_t_gb2312b);
  spr.setTextColor(COL_DIM);
  spr.setCursor(cx - 40, SAFE_T + 155);
  spr.printf("%us 后返回...", (unsigned int)(2 - elapsedMs / 1000));
}
```

- [ ] **Step 5: 提交**

```bash
git add src/usage_display.cpp
git commit -m "feat: enhance thinking display with progress bar, add done/failed screens"
```

---

### Task 4: 更新 main.cpp 状态机与 HTTP 解析

**Files:**
- Modify: `D:\esp32\esp32-usage-monitor\src\main.cpp`

此任务分 3 个子步骤：新增变量 → 增强 HTTP 解析 → 修改主循环。

- [ ] **Step 1: 新增全局状态变量**

在 `s_thinkingSince` 之后（第 24 行后）插入：

```cpp
static int s_thinkingStep = 0;
static char s_thinkingMsg[64] = "";
static int s_totalSteps = 0;
static bool s_showDone = false;
static bool s_showFailed = false;
static uint32_t s_doneSince = 0;
```

- [ ] **Step 2: 增强 `handleHttpStatus()` 解析**

将 `handleHttpStatus()` 函数（第 32-64 行）替换为：

```cpp
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

        // 解析 step=N
        int stepIdx = req.indexOf("step=");
        if (stepIdx >= 0) {
            s_thinkingStep = atoi(req.c_str() + stepIdx + 5);
        }

        // 解析 msg=...
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

        Serial.printf("state=running step=%d msg=%s\n", s_thinkingStep, s_thinkingMsg);

    } else if (req.indexOf("state=done") >= 0) {
        if (s_thinking) {
            s_showDone = true;
            s_doneSince = millis();
        }
        s_thinking = false;
        Serial.println("state=done");

    } else if (req.indexOf("state=failed") >= 0) {
        if (s_thinking) {
            s_showFailed = true;
            s_doneSince = millis();
        }
        s_thinking = false;
        Serial.println("state=failed");
    }

    client.println("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nOK");
    delay(10);
    client.stop();
}
```

- [ ] **Step 3: 修改主循环 `loop()` 的绘制部分**

将第 224-235 行的绘制分支替换为：

```cpp
    // Draw
    if (s_easterState == 1) {
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
    } else {
        if (s_showTime) {
            usageDisplayDrawTime(WiFi.localIP().toString().c_str(), s_timeValid);
        } else {
            usageDisplayDraw(s_usage, WiFi.localIP().toString().c_str());
        }
    }
```

- [ ] **Step 4: 更新 PWR 按键逻辑**

将第 211-220 行的 PWR 按键处理替换为（新增对 done/failed 画面的取消支持）：

```cpp
    // PWR / KEY1: dismiss overlays or toggle time
    if (hwBtnA().wasPressed) {
        if (s_easterState == 1) {
            stopEaster();
        }
        if (s_showDone || s_showFailed) {
            s_showDone = false;
            s_showFailed = false;
        } else if (s_thinking) {
            s_thinking = false;
        } else {
            s_showTime = !s_showTime;
        }
    }
```

- [ ] **Step 5: 编译验证**

```bash
pio run -e esp32-usage-monitor
```

预期：编译成功，无错误。

- [ ] **Step 6: 提交**

```bash
git add src/main.cpp
git commit -m "feat: add step/progress HTTP parsing, done/failed state machine"
```

---

### Task 5: 集成测试

- [ ] **Step 1: 上传固件到 ESP32**

```bash
pio run -e esp32-usage-monitor -t upload
```

- [ ] **Step 2: 手动测试 HTTP 端点**

在浏览器或 curl 中依次测试：

```powershell
# 开始思考
curl.exe -s "http://192.168.31.243/?state=running"

# 更新步骤 (模拟工具调用)
curl.exe -s "http://192.168.31.243/?state=running&step=1&msg=read_file"

# 更新步骤
curl.exe -s "http://192.168.31.243/?state=running&step=2&msg=search_code"

# 思考完成
curl.exe -s "http://192.168.31.243/?state=done&steps=2"

# 等待 2 秒观察自动返回

# 测试失败
curl.exe -s "http://192.168.31.243/?state=running"
curl.exe -s "http://192.168.31.243/?state=failed"
```

预期行为：
- `state=running` → 屏幕显示思考画面（Logo 闪烁 + 省略号动画 + 进度条滚动）
- `step=1&msg=read_file` → 步骤数更新为 1，描述显示 "read_file"
- `state=done` → 屏幕显示绿色 "OK 思考完成" + 统计，2 秒后自动回到用量页面
- `state=failed` → 屏幕显示红色 "X 任务失败"，2 秒后自动回到用量页面
- PWR 按键 → 可随时取消 thinking/done/failed 画面

- [ ] **Step 3: 启动 OpenCode 验证插件自动触发**

启动 OpenCode 并输入任意任务，观察 ESP32 屏幕是否自动切换。

- [ ] **Step 4: 提交** (如有微调)

```bash
git add -A
git commit -m "test: manual integration test passes"
```
