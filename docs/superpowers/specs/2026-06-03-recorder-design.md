# 录音机功能 - 设计文档

## 概述

在现有 ESP32 用量监视器固件上，新增录音机功能。按 KEY2 开始录音，通过 WebSocket 流式传输 PCM 音频到 PC，PC 端接收后保存为 WAV 文件。替换现有 STT（语音转文字）功能。

## 系统架构

```
ESP32 (Waveshare 2.16)                PC (Python)
┌──────────────────┐                 ┌──────────────────────┐
│  I2S 麦克风采集    │ ──WebSocket──→ │  recorder_server.py  │
│  (16kHz/16bit/mono)│   PCM流式传输   │  (asyncio+websockets) │
│                   │   + start/stop  │          │            │
│  KEY2 控制录音     │                 │  保存 WAV 到磁盘      │
│  开始/停止         │ ←────text────  │  回复保存结果          │
│                   │   saved/error   │                      │
│  AMOLED 状态显示    │                 └──────────────────────┘
│  (待机/录音/上传/   │
│   完成/失败)       │
└──────────────────┘
```

## WebSocket 协议

### ESP32 → PC

| 帧类型 | 内容 | 说明 |
|--------|------|------|
| 文本 | `{"type":"start"}` | 开始录音 |
| 二进制 | PCM 原始数据 | 16kHz/16bit/mono，每帧 640 字节（20ms） |
| 文本 | `{"type":"stop"}` | 停止录音 |

### PC → ESP32

| 帧类型 | 内容 | 说明 |
|--------|------|------|
| 文本 | `{"type":"saved","path":"rec_20260603_143025.wav"}` | 保存成功 |
| 文本 | `{"type":"error","msg":"..."}` | 保存失败 |

## ESP32 固件设计

### 新增文件

#### `src/voice/ws_client.h/.cpp` — WebSocket 客户端

- 使用 ESP-IDF 原生 `esp_websocket_client.h`
- 接口：
  - `wsConnect(host, port)` — 连接 WebSocket 服务器
  - `wsSendBin(data, len)` — 发送二进制 PCM 数据
  - `wsSendText(json)` — 发送文本控制消息
  - `wsIsConnected()` — 检查连接状态
  - `wsDisconnect()` — 断开连接
  - `wsSetCallback(cb)` — 设置消息回调（接收 PC 回复）

#### `src/voice/audio_recorder.h/.cpp` — 录音机状态机

状态枚举：
```cpp
enum class RecorderState {
    Idle,
    Recording,
    Uploading,    // 等待 PC 回复
    Done,
    Failed
};
```

接口：
- `recorderInit()` — 初始化
- `recorderStart()` — 开始录音（连接 WebSocket → 发 start → 启动 I2S RX）
- `recorderTick()` — 每循环调用：从 I2S 读取数据并发送 WebSocket
- `recorderStop()` — 停止录音（停止 I2S RX → 发 stop → 等回复）
- `recorderReset()` — 重置状态
- `recorderGetState()` — 返回当前状态

### 修改文件

#### `main.cpp`

- 移除 STT 相关代码（`#include "voice/stt.h"`，`SttContext s_stt`，STT tick 逻辑）
- 添加录音机状态机：
  - `#include "voice/audio_recorder.h"`
  - `RecorderContext s_recorder;`
  - 按 KEY2 触发 start/stop（仅在非 thinking、非 wifi 配置时）
  - 每循环调用 recorderTick()
  - 超时处理（30 秒自动停止）
  - PC 回复后更新 Done/Failed 状态

#### `usage_display.h/.cpp`

新增录制 UI 绘制函数：
- `usageDisplayDrawRecorderIdle()` — 用量主界面提示"按 KEY2 录音"
- `usageDisplayDrawRecorderRecording(elapsedMs)` — 录音中界面
- `usageDisplayDrawRecorderUploading()` — 上传中界面
- `usageDisplayDrawRecorderDone(path, elapsedMs)` — 保存成功界面
- `usageDisplayDrawRecorderFailed(elapsedMs)` — 保存失败界面

#### `config.h`

- 无需修改（`pc_host` 和 `pc_port` 已存在）

#### `platformio.ini`

- 无需修改（`-DCONFIG_ESP_WEBSOCKET_CLIENT` 已启用）

### 录音参数

- 采样率：16kHz
- 位深：16bit
- 声道：mono
- 每帧大小：640 字节（20ms）
- 最长录音：30 秒
- PCM 数据不缓存，直接转发

### 用户交互

| 状态 | 显示内容 | 操作 |
|------|---------|------|
| Idle | 用量主界面，底部提示"按 KEY2 录音" | 按 KEY2 → Recording |
| Recording | "录音中..." + 计时(0~30s) + 音量指示条 + "按 KEY2 停止" | 按 KEY2 → Uploading / 30s超时 → Uploading |
| Uploading | "上传中..." + 不定长进度条 | 等待 PC 回复 |
| Done | "已保存到 PC: rec_xxx.wav" | 3秒后自动返回 Idle |
| Failed | "保存失败" | 3秒后自动返回 Idle |

## PC 端设计

### `tools/recorder_server.py`

单文件 Python 脚本。

**功能：**
- 监听 `pc_port` 端口（默认 12345）
- 接收 WebSocket 连接
- 接收 start/stop 控制消息和 PCM 二进制流
- 停止后合成 WAV 文件
- 回复 ESP32 保存结果

**流程：**
```
listen → accept → recv "start" → recv binary frames →
recv "stop" → add WAV header → save to ./recordings/ →
reply {"type":"saved","path":"..."}
```

**文件命名：** `rec_YYYYMMDD_HHMMSS.wav`

**依赖：**
```
websockets>=12.0
```

**命令行参数：**
```bash
python tools/recorder_server.py [--port 12345] [--dir ./recordings]
```

## 实现优先级

1. `ws_client.h/.cpp` — WebSocket 客户端（先验证能连接 PC）
2. `tools/recorder_server.py` — PC 端接收服务（先验证 WebSocket 通信）
3. `audio_recorder.h/.cpp` — 录音机状态机
4. 修改 `main.cpp` — 替换 STT，集成录音机
5. UI 绘制函数 — `usage_display.cpp`

## 约束

- 不使用云 API
- 录音不保存在 ESP32 本地
- 音频格式：16kHz/16bit/mono PCM → WAV
- 若 WiFi 断开或 PC 不可达，显示连接失败提示
