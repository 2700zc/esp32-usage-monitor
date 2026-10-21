#include "hw/imu.h"
#include "hw/pins.h"
#include <Wire.h>
#include <SensorQMI8658.hpp>

static SensorQMI8658 s_qmi;

bool hwImuInit() {
  if (!s_qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, PIN_I2C_SDA, PIN_I2C_SCL)) {
    Serial.println("hwImu: QMI8658 begin failed");
    return false;
  }
  Serial.printf("hwImu: WHO_AM_I=0x%02X chipID=0x%02X\n",
                s_qmi.whoAmI(), s_qmi.getChipID());
  // Reset to clean default state — without this on the 2.16, the chip
  // returned saturated X/Y axis readings (stuck at +2g/+2g).
  s_qmi.reset();
  s_qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_4G,
                            SensorQMI8658::ACC_ODR_125Hz);
  s_qmi.enableAccelerometer();
  // 平放时重力方向不随原地旋转变化，加速度计测不到 90° 转向；这时用陀螺仪
  // z 轴的角速度积分。±1024 dps 留足快速转动的余量，112 Hz 对 50 Hz 轮询足够。
  if (!s_qmi.configGyroscope(SensorQMI8658::GYR_RANGE_1024DPS,
                             SensorQMI8658::GYR_ODR_112_1Hz,
                             SensorQMI8658::LPF_MODE_0) ||
      !s_qmi.enableGyroscope()) {
    Serial.println("hwImu: gyro init failed (flat-rotation disabled)");
  } else {
    Serial.println("hwImu: gyro 1024dps/112Hz enabled");
  }
  return true;
}

bool hwImuReadAccel(float* ax, float* ay, float* az) {
  IMUdata d = {};
  if (!s_qmi.getAccelerometer(d.x, d.y, d.z)) return false;
  if (ax) *ax = d.x;
  if (ay) *ay = d.y;
  // Z is inverted vs M5StickC convention. Smoke 3 verified: screen-up
  // gives raw d.z ≈ -0.94 on this board, but the original face-down
  // detector wants az < -0.7 for face-DOWN. Flip the sign.
  if (az) *az = -d.z;
  return true;
}

bool hwImuReadGyro(float* gx, float* gy, float* gz) {
  IMUdata d = {};
  if (!s_qmi.getGyroscope(d.x, d.y, d.z)) return false;
  // Keep the raw z sign: screen-up gives raw accel z ≈ -1, so sensor +z points
  // into the back of the screen and gz > 0 means the device turns clockwise
  // (seen from the front). Do not apply the accelerometer's z flip here.
  if (gx) *gx = d.x;
  if (gy) *gy = d.y;
  if (gz) *gz = d.z;
  return true;
}

void hwImuAccel(float* ax, float* ay, float* az) {
  if (!hwImuReadAccel(ax, ay, az)) {
    if (ax) *ax = 0;
    if (ay) *ay = 0;
    if (az) *az = 0;
  }
}
