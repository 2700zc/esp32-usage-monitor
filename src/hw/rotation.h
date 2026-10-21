#pragma once
#include "hw/display.h"

// 自动旋转：倾斜时用加速度计判断哪条边朝下；平放在桌面上原地转 90° 时
// 重力方向不变，改用陀螺仪 z 轴检测这次转动。
//
// hwRotationInit() 在 hwInit() 里、IMU 初始化之后调用一次；
// hwRotationPoll() 在主循环里每帧调用，内部按 20ms 节流。
bool hwRotationInit();
bool hwRotationPoll();   // 本次调用是否改变了屏幕方向
ScreenRot hwRotationGet();
