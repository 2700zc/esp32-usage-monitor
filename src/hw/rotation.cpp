#include "hw/rotation.h"
#include "hw/display.h"
#include "hw/imu.h"
#include "hw/pins.h"
#include <Arduino.h>
#include <Preferences.h>
#include <math.h>

#if BOARD_HAS_AUTO_ROTATE

// ── 参数 ──
// 20ms = 50Hz。陀螺仪 112Hz，50Hz 采样做 90° 手势积分够用；再慢会漏掉快速转动。
static const uint32_t POLL_MS        = 20;
// 倾斜方向要稳定这么久才切换，避免端着杯子从传感器前面走过就翻屏。
static const uint32_t SETTLE_MS      = 400;
// 平面内重力分量小于这个值视为平放：加速度计无法判断朝向，交给陀螺仪。
// 0.25g ≈ 离桌面 14.5°；桌面轻微不平不会误判成“倾斜”。
static const float    TILT_MIN       = 0.25f;
// 两个轴的分量接近（45° 附近）时不切换：没有哪条边明确朝下，保持原方向最稳。
static const float    AXIS_RATIO     = 1.25f;
// 陀螺仪静止零偏/噪声门限，单位 dps。
static const float    GYRO_DEADBAND  = 8.0f;
// 角速度回落到门限以下持续这么久，认为这次转动结束，再做 90° 吸附。
static const uint32_t GYRO_END_MS    = 250;
// 累计超过 60° 才吸附到最近的 90°，避免 40° 的晃动就翻屏。
static const float    GYRO_SNAP_DEG  = 60.0f;
static const float    STEP_DEG       = 90.0f;
static const float    MAX_DT         = 0.2f;   // 主循环卡顿时的积分上限

static Preferences s_prefs;
static bool        s_ready        = false;
static uint32_t    s_lastPollMs   = 0;
static int         s_savedRot     = -1;   // 已写进 NVS 的值，避免重复擦写

// 加速度计候选方向
static ScreenRot   s_pending      = ScreenRot::R0;
static uint32_t    s_pendingSince = 0;

// 陀螺仪手势
static bool        s_gyroActive   = false;
static bool        s_gyroHasLast  = false;
static uint32_t    s_gyroLastMs   = 0;
static uint32_t    s_gyroEndMs    = 0;
static float       s_gyroYaw      = 0.0f;   // 本次连续转动累计角度（设备顺时针为正）
static float       s_gyroResidual = 0.0f;   // 跨动作保留的角度零头，支持分两次转完 90°

static ScreenRot loadStored() {
  s_prefs.begin("rot", true);
  uint8_t v = s_prefs.getUChar("q", 0) & 0x03;
  s_prefs.end();
  s_savedRot = v;
  return (ScreenRot)v;
}

static void persist(ScreenRot r) {
  if ((int)r == s_savedRot) return;
  s_prefs.begin("rot", false);
  s_prefs.putUChar("q", (uint8_t)r);
  s_prefs.end();
  s_savedRot = (int)r;
}

static void applyRotation(ScreenRot r) {
  if (r == hwDisplayGetRotation()) return;
  hwDisplaySetRotation(r);
  persist(r);
  Serial.printf("rot: -> %d deg (q=%d)\n", (int)r * 90, (int)r);
}

// 只看平面内重力分量。QMI8658 是右手系：屏幕朝上时原始 accel z ≈ -1，
// 说明 +z 指向屏幕背面。加速度计静止时读的是“向上的那根轴”，官方示例把
// +y 朝上视为正常竖屏，因此 +y 指向屏幕上方、+x 指向屏幕左侧。
//   +y 朝上 = 正常竖屏；-y = 倒置
//   +x 朝上 = 设备顺时针转 90°（左侧边朝上），内容要逆时针转 = R270
//   -x 朝上 = 设备逆时针转 90°，内容要顺时针转 = R90
static ScreenRot classifyTilt(float ax, float ay) {
  float aax = fabsf(ax);
  float aay = fabsf(ay);
  if (aay > aax * AXIS_RATIO) return ay > 0 ? ScreenRot::R0 : ScreenRot::R180;
  if (aax > aay * AXIS_RATIO) return ax > 0 ? ScreenRot::R270 : ScreenRot::R90;
  return hwDisplayGetRotation();
}

static void accelUpdate(float ax, float ay, uint32_t now, bool flat) {
  if (flat) {
    s_pending      = hwDisplayGetRotation();
    s_pendingSince = 0;
    return;
  }
  ScreenRot cand = classifyTilt(ax, ay);
  ScreenRot cur  = hwDisplayGetRotation();
  if (cand == cur) {
    s_pending      = cur;
    s_pendingSince = 0;
    return;
  }
  if (cand != s_pending || s_pendingSince == 0) {
    s_pending      = cand;
    s_pendingSince = now;
    return;
  }
  if (now - s_pendingSince >= SETTLE_MS) {
    applyRotation(cand);
    s_pendingSince = 0;
  }
}

static void gyroReset() {
  s_gyroActive   = false;
  s_gyroHasLast  = false;
  s_gyroYaw      = 0.0f;
  s_gyroResidual = 0.0f;
}

static void gyroUpdate(float gz, uint32_t now) {
  if (!s_gyroHasLast) {
    s_gyroLastMs  = now;
    s_gyroHasLast = true;
    return;
  }
  uint32_t elapsed = now - s_gyroLastMs;
  s_gyroLastMs = now;
  if (elapsed == 0) return;
  float dt = elapsed / 1000.0f;
  if (dt > MAX_DT) dt = MAX_DT;

  bool moving = fabsf(gz) >= GYRO_DEADBAND;
  if (moving) {
    if (!s_gyroActive) {
      s_gyroActive = true;
      s_gyroYaw    = 0.0f;
    }
    s_gyroYaw += gz * dt;
    s_gyroEndMs = now;
    return;
  }
  if (!s_gyroActive || now - s_gyroEndMs < GYRO_END_MS) return;

  // gz > 0：设备顺时针转；内容要逆时针转，所以从枚举里减。
  float total = s_gyroResidual + s_gyroYaw;
  int steps = 0;
  if (fabsf(total) >= GYRO_SNAP_DEG) {
    steps = (int)lroundf(total / STEP_DEG);
  }
  if (steps != 0) {
    int next = ((int)hwDisplayGetRotation() - steps) % 4;
    if (next < 0) next += 4;
    applyRotation((ScreenRot)next);
    total -= steps * STEP_DEG;
  }
  // 没转够 90° 的零头留到下一次；限制幅度防止长时间抖动累积。
  if (total >  60.0f) total =  60.0f;
  if (total < -60.0f) total = -60.0f;
  s_gyroResidual = total;
  s_gyroActive   = false;
  s_gyroYaw      = 0.0f;
}

bool hwRotationInit() {
  ScreenRot stored = loadStored();
  float ax = 0, ay = 0, az = 0;
  bool haveData = false;

  // 传感器刚使能后的前几个寄存器的值可能还是 0。等第一份非零样本，
  // 否则开机第一帧会被误判成“平放”，方向要等主循环下一次轮询才纠正。
  for (int attempt = 0; attempt < 6; attempt++) {
    if (hwImuReadAccel(&ax, &ay, &az)) {
      haveData = true;
      if (fabsf(ax) + fabsf(ay) + fabsf(az) > 0.02f) break;
    }
    delay(20);
  }
  if (!haveData) {
    Serial.println("rot: no accelerometer, auto-rotate disabled");
    return false;
  }

  float inplane = sqrtf(ax * ax + ay * ay);
  if (inplane < 0.01f && fabsf(az) < 0.01f) {
    // 等不到数据也别卡住启动：先用上次记住的方向，主循环接着轮询。
    Serial.println("rot: IMU data not ready, using stored orientation");
    hwDisplaySetRotation(stored);
    persist(stored);
    s_ready = true;
    return true;
  }

  // 平放时加速度计给不出绝对方位，用上次记住的方向；倾斜时以重力为准。
  // 两条边接近 45° 时重力也分不清该取哪一个 90° 方向，这时同样信上次的结果，
  // 避免开机先画 R0、400ms 后再翻一次的闪动。
  float aax = fabsf(ax);
  float aay = fabsf(ay);
  bool ambiguous = !(aay > aax * AXIS_RATIO || aax > aay * AXIS_RATIO);
  ScreenRot initial = (inplane < TILT_MIN || ambiguous) ? stored : classifyTilt(ax, ay);
  hwDisplaySetRotation(initial);
  persist(initial);
  s_ready = true;
  Serial.printf("rot: init %d deg (ax=%.2f ay=%.2f az=%.2f, stored=%d deg)\n",
                (int)initial * 90, ax, ay, az, (int)stored * 90);
  return true;
}

bool hwRotationPoll() {
  if (!s_ready) return false;
  uint32_t now = millis();
  if (now - s_lastPollMs < POLL_MS) return false;
  s_lastPollMs = now;

  float ax = 0, ay = 0, az = 0;
  if (!hwImuReadAccel(&ax, &ay, &az)) return false;

  float inplane = sqrtf(ax * ax + ay * ay);
  bool  flat    = inplane < TILT_MIN;
  ScreenRot before = hwDisplayGetRotation();

  accelUpdate(ax, ay, now, flat);

  if (!flat) {
    // 倾斜时加速度计给出绝对方位，陀螺仪手势清零，防止两套状态互相干扰。
    gyroReset();
  } else {
    float gx = 0, gy = 0, gz = 0;
    if (hwImuReadGyro(&gx, &gy, &gz)) {
      gyroUpdate(gz, now);
    } else {
      gyroReset();
    }
  }
  return hwDisplayGetRotation() != before;
}

ScreenRot hwRotationGet() { return hwDisplayGetRotation(); }

#else  // !BOARD_HAS_AUTO_ROTATE

bool hwRotationInit() { return false; }
bool hwRotationPoll() { return false; }
ScreenRot hwRotationGet() { return hwDisplayGetRotation(); }

#endif
