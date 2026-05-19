#include "audio_recorder.h"
#include "hw/pins.h"
#include <Arduino.h>
#include <Wire.h>
#include <cstring>
#include <driver/i2s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static constexpr int SR = 16000;
static constexpr size_t BUF_MAX = SR * 2 * 60;

static uint8_t* s_buf = nullptr;
static volatile size_t s_written = 0;
static volatile bool s_running = false;
static TaskHandle_t s_recTask = nullptr;

// ── ES7210 I2C helpers ──────────────────────────────────────────────────────

static void es7210Write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(0x40);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static bool es7210Init() {
  delay(10);
  es7210Write(0x00, 0xFF); delay(10);
  es7210Write(0x00, 0x00); delay(10);

  es7210Write(0x01, 0x30);
  es7210Write(0x02, 0x12);
  es7210Write(0x03, 0x03);
  es7210Write(0x14, 0x08);
  es7210Write(0x15, 0x10);
  es7210Write(0x07, 0x30);
  es7210Write(0x08, 0x33);
  es7210Write(0x09, 0x30);

  es7210Write(0x28, 0x00);
  es7210Write(0x29, 0x00);

  es7210Write(0x20, 0x00);
  es7210Write(0x21, 0x00);
  es7210Write(0x24, 0x00);
  es7210Write(0x25, 0x00);

  return true;
}

// ── FreeRTOS recording task ─────────────────────────────────────────────────

static void recTask(void*) {
  s_written = 0;
  s_running = true;
  int16_t dma[256];
  while (s_running && s_written + sizeof(dma) <= BUF_MAX) {
    size_t read = 0;
    if (i2s_read(I2S_NUM_0, dma, sizeof(dma), &read, portMAX_DELAY) == ESP_OK && read > 0) {
      memcpy(s_buf + s_written, dma, read);
      s_written += read;
    }
  }
  s_running = false;
  vTaskDelete(nullptr);
}

// ── Public API ──────────────────────────────────────────────────────────────

bool recorderInit() {
  if (s_buf) return true;
  s_buf = (uint8_t*)ps_malloc(BUF_MAX);
  if (!s_buf) return false;

  size_t free8 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  Serial.printf("recorder: PSRAM free=%u B, buf=%u B\n", free8, BUF_MAX);

  es7210Init();
  return true;
}

void recorderStart() {
  if (s_running) return;

  // Tear down I2S_NUM_0 TX (beep driver)
  i2s_driver_uninstall(I2S_NUM_0);

  // Reinstall I2S_NUM_0 in RX mode for microphone capture
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = SR;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 4;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;
  cfg.fixed_mclk = SR * 256;
  cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK) {
    Serial.println("recorder: I2S RX install failed");
    return;
  }

  i2s_pin_config_t pins = {};
  pins.mck_io_num   = PIN_I2S_MCLK;
  pins.bck_io_num   = PIN_I2S_BCLK;
  pins.ws_io_num    = PIN_I2S_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num  = PIN_I2S_DI;

  if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
    Serial.println("recorder: I2S set pin failed");
    i2s_driver_uninstall(I2S_NUM_0);
    return;
  }

  xTaskCreatePinnedToCore(recTask, "rec", 4096, nullptr, 5, &s_recTask, tskNO_AFFINITY);
}

void recorderStop() {
  if (!s_running) return;

  s_running = false;
  if (s_recTask) {
    vTaskDelete(s_recTask);
    s_recTask = nullptr;
  }

  // Tear down I2S_NUM_0 RX (recording driver)
  i2s_driver_uninstall(I2S_NUM_0);

  // Reinstall I2S_NUM_0 TX to restore beep functionality
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = SR;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 4;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;
  cfg.fixed_mclk = SR * 256;
  cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK) {
    Serial.println("recorder: I2S TX reinstall failed");
    return;
  }

  i2s_pin_config_t pins = {};
  pins.mck_io_num   = PIN_I2S_MCLK;
  pins.bck_io_num   = PIN_I2S_BCLK;
  pins.ws_io_num    = PIN_I2S_WS;
  pins.data_out_num = PIN_I2S_DO;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;

  if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
    Serial.println("recorder: I2S TX pin set failed");
  }
}

bool recorderIsRunning() {
  return s_running;
}

const uint8_t* recorderData() {
  return s_buf;
}

size_t recorderLen() {
  return s_written;
}
