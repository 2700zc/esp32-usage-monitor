// src/hw/display.h
#pragma once
#include <stdint.h>
#include <Arduino_GFX_Library.h>
#include "hw/pins.h"   // provides BOARD_HW_W, BOARD_HW_H, BOARD_SAFE_INSET

// 内容相对 R0（当前竖屏）的顺时针旋转角度。屏幕是方形，旋转只换扫描方向，
// 不改画布尺寸，也不触发重排。
enum class ScreenRot : uint8_t {
  R0   = 0,
  R90  = 1,
  R180 = 2,
  R270 = 3,
};

constexpr int HW_W       = BOARD_HW_W;
constexpr int HW_H       = BOARD_HW_H;

// Logical-canvas safe-draw region for the rounded/circular AMOLED bezel.
constexpr int SAFE_INSET = BOARD_SAFE_INSET;
constexpr int SAFE_L     = SAFE_INSET;
constexpr int SAFE_T     = SAFE_INSET;
constexpr int SAFE_R     = HW_W - SAFE_INSET;
constexpr int SAFE_B     = HW_H - SAFE_INSET;
constexpr int SAFE_W     = HW_W - 2 * SAFE_INSET;
constexpr int SAFE_H     = HW_H - 2 * SAFE_INSET;

bool hwDisplayInit();
void hwDisplayPush();
void hwDisplayBrightness(uint8_t lvl_0_4);
void hwDisplaySleep(bool off);
Arduino_Canvas* hwCanvas();

// 只在支持自动旋转的板型上生效；其他板型是 no-op / 恒为 R0。
ScreenRot hwDisplayGetRotation();
void hwDisplaySetRotation(ScreenRot r);
