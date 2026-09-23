#include "hw/gesture.h"
#include "hw/input.h"
#include "hw/display.h"
#include <Arduino.h>

// 阈值用画布坐标（184×224）而不是物理坐标，这样 letterbox / 缩放 / 偏移
// 各板型通用。主方向位移 35px 约占画布短边 19%，是「明显在滑」而不是「点歪了」。
static const int      SWIPE_MIN_PX  = 35;
static const uint32_t SWIPE_MAX_MS  = 800;
static const int      SWIPE_RATIO   = 3;    // 主方向至少是次方向的 1.5 倍（3/2）
static const int      SWIPE_EDGE_PX = 24;   // 起手太靠边不算，避免边缘误触

static bool     s_active;
static int16_t  s_startX, s_startY;
static int16_t  s_lastX, s_lastY;
static uint32_t s_startMs;

SwipeDir hwSwipePoll() {
  const HwTouch& t = hwTouch();

  if (t.justPressed) {
    s_active  = true;
    s_startX  = s_lastX = t.x;
    s_startY  = s_lastY = t.y;
    s_startMs = millis();
  }

  if (!s_active) return SwipeDir::None;

  if (t.down) {
    // 按住期间持续跟踪终点；坐标可能比起点更早到（先向右再拉回）
    s_lastX = t.x;
    s_lastY = t.y;
    return SwipeDir::None;
  }

  // down 为 false 即视为抬手。不依赖 justReleased：轮询在无中断且手指未按下
  // 时会被跳过，那就会漏掉抬起那一帧。
  s_active = false;

  if (millis() - s_startMs > SWIPE_MAX_MS) return SwipeDir::None;

  int dx = (int)s_lastX - (int)s_startX;
  int dy = (int)s_lastY - (int)s_startY;
  int adx = dx < 0 ? -dx : dx;
  int ady = dy < 0 ? -dy : dy;

  // 判定用 dy：屏幕经 MADCTL 旋转 90°（MV 位，见 BOARD_CO5300_MADCTL），触摸
  // 上报的 x/y 与画布逻辑坐标是转置的——input.cpp 里的 setMirrorXY(true,true)
  // 只等价于 180° 镜像，做不到转置。所以横向滑动表现为上报 y 的变化。
  if (ady < SWIPE_MIN_PX) return SwipeDir::None;
  if (ady * 2 < SWIPE_RATIO * adx) return SwipeDir::None;
  if (s_startY < SWIPE_EDGE_PX || s_startY > HW_H - SWIPE_EDGE_PX) return SwipeDir::None;

  SwipeDir dir = dy < 0 ? SwipeDir::Left : SwipeDir::Right;
  // 打出 dx/dy 以便核对轴向与阈值；确认稳定后可只在失败时打
  Serial.printf("swipe: dx=%d dy=%d -> %s\n", dx, dy,
                dir == SwipeDir::Left ? "next" : "prev");
  return dir;
}
