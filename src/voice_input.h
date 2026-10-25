#pragma once
#include <Arduino.h>

// BOOT 键语音输入。
//
// 按下 BOOT 开始录音，再按一次结束；设备把 16 kHz / 16 bit 单声道 PCM
// POST 到 PC 上 tools/agent-bridge/bridge.py 的 /asr 接口。桥接调用火山
// 语音识别，把结果粘贴到 PC 当前前台窗口的光标处。
//
// 这里只负责采集、上传和状态界面；识别与键盘注入都在 PC 端完成，设备
// 不保存任何云 API 密钥。

void voiceInputInit();

// 主循环每帧调用：更新桥接地址、令牌与网络状态。
void voiceInputSetServer(const char* host, uint16_t port, const char* token,
                         bool wifiConnected);

// 只在主界面生效。设置页 / WiFi 配置页里 BOOT 仍归这些页面使用
// （否则软键盘和「完成」没法操作）。
void voiceInputHandleButton(bool allowToggle);

// 状态推进：60 秒自动结束、结果提示 1.8 秒后回到主界面。
void voiceInputTick();

// true = 主循环应绘制语音全屏界面并暂停普通页面逻辑。
bool voiceInputActive();
void voiceInputDraw();
// 语音界面退出后请求主页面立刻重绘一次。
bool voiceInputConsumeRedraw();
