#pragma once
#include <stdint.h>

// BLE HID 键盘（实验性按键页的右 Alt 通道）。
//
// 为什么需要它：豆包输入法的语音快捷键会过滤 SendInput 注入的按键（它的
// VoiceKeyHookProc 里带 injected 标记），但物理键盘的右 Alt 有效。让 ESP32
// 以真正的 BLE 键盘身份发 HID 报文，主机侧无法区分它和物理键盘。
//
// 需要先在 Windows 蓝牙设置里配对一次，设备名见 bleHidName()。配对信息由
// Bluedroid 存在 NVS 里，之后开机自动重连。
bool        bleHidInit();
bool        bleHidReady();       // 已连接且配对/加密完成
const char* bleHidName();

// 右 Alt 按下 -> 停 120ms -> 抬起。真人按键的持续时间，避免主机把
// "同一批报告里按下又抬起"当成抖动。
void        bleHidTapRightAlt();

// 连接期间定期发一个空报告，保持 HID 通知通道活着（Windows 空闲时可能
// 取消订阅，之后 notify 会被静默丢弃）。主循环每帧调用一次，内部限流。
void        bleHidTick();
