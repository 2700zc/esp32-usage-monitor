#include "hw/gesture.h"
#include "hw/input.h"
#include "hw/display.h"
#include "hw/rotation.h"
#include <Arduino.h>

// 阈值用画布坐标（184×224）而不是物理坐标，这样 letterbox / 缩放 / 偏移
// 各板型通用。主方向位移 35px 约占画布短边 19%，是「明显在滑」而不是「点歪了」。
//
// 手势在位移越过阈值的那一帧就结算，不再等抬手：
//   - 旧实现要等手指离开屏幕才判方向，而抬手事件可能落在一次慢绘制/网络请求里；
//     即使等到了，800ms 的时长上限也会把跨过卡顿的那次滑动判成无效。
//   - 现在手指还在屏幕上就切页，用户立刻看到反馈；抬手只负责复位状态。
// SWIPE_ARM_MS 只用来放弃「按住很久后地漂移」，不参与正常滑动的判定。
static const int      SWIPE_MIN_PX  = 35;
static const uint32_t SWIPE_ARM_MS  = 2000;
static const int      SWIPE_RATIO   = 3;    // 主方向至少是次方向的 1.5 倍（3/2）
static const int      SWIPE_EDGE_PX = 24;   // 起手太靠边不算，避免边缘误触

static bool      s_active;
static bool      s_fired;
static int16_t   s_startX, s_startY;
static int16_t   s_lastX, s_lastY;
static uint32_t  s_startMs;
static ScreenRot s_rotAtStart;

// 横向滑动在 R0 下表现为上报 y 的变化：CST9217 的原始 x/y 与画布逻辑坐标是
// 转置的（input.cpp 的 setMirrorXY(true,true) 只等价于 180° 镜像，做不到转置）。
// 自动旋转后视觉上的「横向」落到另一条上报轴上，所以按旋转角选轴和符号：
//   R0   : dy < 0 -> 下一页
//   R90  : dx > 0 -> 下一页
//   R180 : dy > 0 -> 下一页
//   R270 : dx < 0 -> 下一页
static SwipeDir evaluateSwipe() {
  int dx = (int)s_lastX - (int)s_startX;
  int dy = (int)s_lastY - (int)s_startY;
  int adx = dx < 0 ? -dx : dx;
  int ady = dy < 0 ? -dy : dy;

  bool horizY = (s_rotAtStart == ScreenRot::R0 || s_rotAtStart == ScreenRot::R180);
  int along   = horizY ? dy : dx;
  int across  = horizY ? dx : dy;
  int aalong  = along  < 0 ? -along  : along;
  int aacross = across < 0 ? -across : across;

  if (aalong < SWIPE_MIN_PX) return SwipeDir::None;
  if (aalong * 2 < SWIPE_RATIO * aacross) return SwipeDir::None;

  // 起手坐标也要用视觉横向那条轴。注意 input.cpp 的 letterbox 映射是转置的：
  // s_tp.y 实际落在 0..223（HW_H），s_tp.x 落在 0..183（HW_W），这里按上报
  // 轴的真实范围做边缘排除，保持 R0 下原有的手感。
  int startAlong = horizY ? s_startY : s_startX;
  int extent     = horizY ? HW_H : HW_W;
  if (startAlong < SWIPE_EDGE_PX || startAlong > extent - SWIPE_EDGE_PX)
    return SwipeDir::None;

  bool next;
  switch (s_rotAtStart) {
    case ScreenRot::R0:   next = along < 0; break;
    case ScreenRot::R90:  next = along > 0; break;
    case ScreenRot::R180: next = along > 0; break;
    default:              next = along < 0; break;  // R270
  }

  SwipeDir dir = next ? SwipeDir::Left : SwipeDir::Right;
  Serial.printf("swipe: rot=%d dx=%d dy=%d -> %s\n", (int)s_rotAtStart * 90,
                dx, dy, next ? "next" : "prev");
  return dir;
}

SwipeDir hwSwipePoll() {
  const HwTouch& t = hwTouch();

  if (t.justPressed) {
    s_active     = true;
    s_fired      = false;
    s_startX     = s_lastX = t.x;
    s_startY     = s_lastY = t.y;
    s_startMs    = millis();
    s_rotAtStart = hwRotationGet();
  }

  if (!s_active) return SwipeDir::None;

  // 转动设备时手指还按在屏幕上：起点和终点不在同一坐标系，这次手势作废，
  // 否则旋转瞬间会被判成一次横滑。
  if (hwRotationGet() != s_rotAtStart) {
    s_active = false;
    s_fired  = true;
    return SwipeDir::None;
  }

  if (t.down) {
    // 按住期间持续跟踪终点；坐标可能比起点更早到（先向右再拉回）
    s_lastX = t.x;
    s_lastY = t.y;
    if (!s_fired && millis() - s_startMs <= SWIPE_ARM_MS) {
      SwipeDir dir = evaluateSwipe();
      if (dir != SwipeDir::None) {
        s_fired = true;
        return dir;
      }
    }
    return SwipeDir::None;
  }

  // 抬手。input.cpp 的抬起有 40ms 去抖，所以通常已经在上面的分支里结算过了；
  // 这里兜住「最后一帧才越过阈值、还没被采到就抬手」的情况。
  //
  // 只有真的判出方向才置 s_fired：它表示"这次按下已经结算成滑动"。抬手时
  // 无条件置真的话，"没滑动的按下"也会被标成已结算，语义就不对了。
  s_active = false;
  if (!s_fired && millis() - s_startMs <= SWIPE_ARM_MS) {
    SwipeDir dir = evaluateSwipe();
    if (dir != SwipeDir::None) {
      s_fired = true;
      return dir;
    }
  }
  return SwipeDir::None;
}
