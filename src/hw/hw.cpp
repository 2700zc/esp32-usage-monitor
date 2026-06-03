#include "hw/hw.h"
#include <Arduino.h>
#include <Wire.h>

static bool s_initOk = true;

static void die(const char* what) {
  Serial.printf("hwInit FAIL: %s\n", what);
  s_initOk = false;
}

bool hwInitOk() { return s_initOk; }

void hwInit() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== esp32-usage-monitor boot ===");

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(400000);

  if (!hwExpanderInit())  die("expander");
#if BOARD_LCD_RST_VIA_PMU
  if (!hwPowerInit())      die("power");
  hwPmuRef()->disableALDO3();
  delay(50);
  hwPmuRef()->enableALDO3();
  delay(50);
#endif
  hwExpanderResetSequence();
  if (!hwDisplayInit())    die("display");
#if !BOARD_LCD_RST_VIA_PMU
  if (!hwPowerInit())      die("power");
#endif
  if (!hwInputInit())      die("input");
  if (!hwImuInit())        die("imu");
  if (!hwRtcInit())        die("rtc");

  Serial.println(s_initOk ? "hwInit OK" : "hwInit DONE (some failures)");
}