#include "voice_input.h"

#include "hw/display.h"
#include "hw/input.h"
#include "hw/pins.h"
#include "hw/power.h"
#include "hw/es7210.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>

#define spr (*hwCanvas())

// ── 录音参数 ──
// 16 kHz / 16 bit / 单声道：火山「大模型录音文件识别」直接吃这个格式，
// 60 秒约 1.9 MB，放 PSRAM 里绰绰有余。
static const uint32_t VOICE_SAMPLE_RATE  = 16000;
static const uint32_t VOICE_MAX_SECONDS  = 60;
static const uint32_t VOICE_MAX_SAMPLES  = VOICE_SAMPLE_RATE * VOICE_MAX_SECONDS;
static const uint32_t VOICE_MAX_BYTES    = VOICE_MAX_SAMPLES * 2;
static const uint32_t VOICE_FRAME_MS     = 20;
static const uint32_t VOICE_FRAME_SAMPLES = VOICE_SAMPLE_RATE * VOICE_FRAME_MS / 1000;
static const i2s_port_t VOICE_I2S_PORT   = I2S_NUM_1;
static const uint32_t VOICE_RESULT_MS    = 1800;   // 结果提示停留时间

// ── 配色：与 usage_display 同一套 ──
static const uint16_t C_BG     = 0x0000;
static const uint16_t C_TEXT   = 0xF7BF;
static const uint16_t C_TEXT2  = 0x8C73;
static const uint16_t C_BLUE   = 0x4C7F;
static const uint16_t C_GREEN  = 0x2E6E;
static const uint16_t C_AMBER  = 0xFD20;
static const uint16_t C_RED    = 0xF800;
static const uint16_t C_OFF    = 0x2104;

enum class VState : uint8_t { Idle, Recording, Uploading, Done, Error };

static volatile VState s_state = VState::Idle;
static volatile bool   s_stopReq = false;
static volatile uint32_t s_startedMs = 0;
static volatile uint32_t s_endedMs = 0;
static volatile uint16_t s_level = 0;          // 0..100，界面电平条
static volatile bool   s_redraw = false;

static int16_t* s_pcm = nullptr;
static volatile uint32_t s_samples = 0;
static TaskHandle_t s_task = nullptr;

static char s_host[48] = "";
static uint16_t s_port = 0;
static char s_token[64] = "";
static volatile bool s_wifi = false;

// 录音开始时把桥接信息拷一份，任务里不再碰主循环的字符串。
static char s_upHost[48] = "";
static uint16_t s_upPort = 0;
static char s_upToken[64] = "";

static char s_text[192] = "";
static char s_err[96] = "";
static portMUX_TYPE s_textMux = portMUX_INITIALIZER_UNLOCKED;
static bool s_initOk = false;

// ── 小工具 ──

static int textWidth(const char* s, int cell, int size) {
  int w = 0;
  for (const unsigned char* p = (const unsigned char*)s; *p; ) {
    if (*p < 0x80)               { w += cell / 2; p += 1; }
    else if ((*p & 0xE0) == 0xC0) { w += cell;     p += 2; }
    else if ((*p & 0xF0) == 0xE0) { w += cell;     p += 3; }
    else                          { w += cell;     p += 4; }
  }
  return w * size;
}

static void drawCentered(const char* s, int y, int cell, int size, uint16_t color) {
  spr.setFont(cell == 16 ? u8g2_font_wqy16_t_gb2312b : u8g2_font_wqy12_t_gb2312b);
  spr.setTextSize(size);
  spr.setTextColor(color);
  int w = textWidth(s, cell, size);
  spr.setCursor((HW_W - w) / 2, y);
  spr.print(s);
  spr.setTextSize(1);
}

// 按像素宽度折行，最多 maxLines 行。中文/英文混排都能用。
static void drawWrapped(const char* text, int x, int y, int maxW, int lineH,
                        int maxLines, uint16_t color) {
  spr.setFont(u8g2_font_wqy12_t_gb2312b);
  spr.setTextColor(color);
  char line[128];
  int li = 0;
  const char* p = text;
  while (*p && li < maxLines) {
    int n = 0;
    while (*p) {
      if (*p == '\n') { p++; break; }
      unsigned char c = (unsigned char)*p;
      int len = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 : 4;
      if (n + len >= (int)sizeof(line) - 1) break;
      memcpy(line + n, p, len);
      n += len;
      line[n] = 0;
      if (textWidth(line, 12, 1) > maxW) {
        line[n - len] = 0;
        n -= len;
        break;
      }
      p += len;
    }
    if (n > 0) {
      spr.setCursor(x, y + li * lineH);
      spr.print(line);
      li++;
    } else if (*p) {
      p++;   // 单个字符就超过 maxW，跳过它，避免死循环
    }
  }
}

static void drawLevel(int y) {
  const int bars = 18;
  const int bw = 5;
  const int gap = 3;
  const int total = bars * bw + (bars - 1) * gap;
  const int x0 = (HW_W - total) / 2;
  const int on = (int)s_level * bars / 100;
  for (int i = 0; i < bars; i++) {
    const int h = 8 + (i % 4) * 3;
    uint16_t c = C_OFF;
    if (i < on) {
      c = (i >= bars * 4 / 5) ? C_RED : (i >= bars * 3 / 5) ? C_AMBER : C_GREEN;
    }
    spr.fillRoundRect(x0 + i * (bw + gap), y - h / 2, bw, h, 2, c);
  }
}

static void fail(const char* msg) {
  strlcpy(s_err, msg ? msg : "未知错误", sizeof(s_err));
  s_state = VState::Error;
  s_endedMs = millis();
  s_level = 0;
  Serial.printf("voice: error: %s\n", s_err);
}

// ── 采集/上传任务 ──

static void uploadRecording() {
  char url[192];
  snprintf(url, sizeof(url), "http://%s:%u/asr?tok=%s",
           s_upHost, (unsigned)s_upPort, s_upToken);

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(30000);
  http.setConnectTimeout(5000);
  if (!http.begin(client, url)) {
    fail("HTTP 初始化失败");
    return;
  }
  http.addHeader("Content-Type", "application/octet-stream");
  http.addHeader("X-Sample-Rate", "16000");
  http.addHeader("X-Channels", "1");
  http.addHeader("X-Bits", "16");

  const size_t bytes = (size_t)s_samples * 2;
  const int code = http.POST((uint8_t*)s_pcm, bytes);
  String body = http.getString();
  http.end();

  Serial.printf("voice: POST %u B -> %d, resp %u B\n",
                (unsigned)bytes, code, (unsigned)body.length());

  if (code <= 0) {
    fail("连不上电脑桥接");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    snprintf(s_err, sizeof(s_err), "电脑返回异常 (HTTP %d)", code);
    s_state = VState::Error;
    s_endedMs = millis();
    return;
  }

  const char* error = doc["error"] | "";
  if (code != 200 || !(doc["ok"] | false)) {
    fail(error[0] ? error : "识别失败");
    return;
  }

  const char* text = doc["text"] | "";
  if (!text[0]) {
    fail("没有识别到文字");
    return;
  }

  portENTER_CRITICAL(&s_textMux);
  strlcpy(s_text, text, sizeof(s_text));
  portEXIT_CRITICAL(&s_textMux);

  s_state = VState::Done;
  s_endedMs = millis();
  s_level = 0;
  Serial.printf("voice: transcript %u chars\n", (unsigned)strlen(text));
}

static void voiceTask(void*) {
  static int16_t frame[VOICE_FRAME_SAMPLES * 2];

  // 丢掉 I2S 刚启动的前几帧：DMA 里可能有全零或启动爆音，直接录进去会
  // 让 ASR 把开头的爆音当成语音。5 帧 ≈ 100 ms。
  for (int i = 0; i < 5; i++) {
    size_t drop = 0;
    i2s_read(VOICE_I2S_PORT, frame, sizeof(frame), &drop, pdMS_TO_TICKS(100));
  }

  bool chosen = false;
  bool stereo = false;
  int  channel = 0;
  uint64_t energy0 = 0, energy1 = 0;
  uint32_t pairsSeen = 0, pairsDiff = 0;

  while (!s_stopReq && s_samples < VOICE_MAX_SAMPLES) {
    size_t bytes = 0;
    esp_err_t r = i2s_read(VOICE_I2S_PORT, frame, sizeof(frame), &bytes,
                           pdMS_TO_TICKS(100));
    if (r != ESP_OK || bytes < 4) continue;

    const int pairs = (int)(bytes / 4);   // 左右各 16 bit
    uint32_t peak = 0;

    // 前 0.25 秒判断到底是两路真麦克风（左右不同），还是同一路被复制成
    // 两个 slot（左右几乎相同）。顺便挑能量大的那一路。
    if (!chosen) {
      for (int i = 0; i < pairs; i++) {
        int16_t l = frame[i * 2];
        int16_t rr = frame[i * 2 + 1];
        energy0 += (uint32_t)abs((int)l);
        energy1 += (uint32_t)abs((int)rr);
        if (abs((int)l - (int)rr) > 128) pairsDiff++;
        pairsSeen++;
      }
      if (pairsSeen >= 4000) {
        stereo = pairsDiff > pairsSeen / 10;
        channel = energy1 > energy0 ? 1 : 0;
        chosen = true;
        Serial.printf("voice: input %s, ch=%d (e0=%llu e1=%llu diff=%u/%u)\n",
                      stereo ? "stereo" : "mono", channel,
                      (unsigned long long)energy0, (unsigned long long)energy1,
                      (unsigned)pairsDiff, (unsigned)pairsSeen);
      }
    }

    for (int i = 0; i < pairs && s_samples < VOICE_MAX_SAMPLES; i++) {
      int16_t l = frame[i * 2];
      int16_t rr = frame[i * 2 + 1];
      int16_t v;
      if (!chosen)       v = l;                          // 先按左路存，选定后不再改
      else if (stereo)   v = (channel == 0) ? l : rr;
      else               v = (int16_t)(((int)l + (int)rr) / 2);

      s_pcm[s_samples++] = v;
      uint32_t a = (uint32_t)abs((int)v);
      if (a > peak) peak = a;
    }

    int pct = (int)(peak * 100 / 32768);
    if (pct > 100) pct = 100;
    s_level = (uint16_t)((s_level * 3 + pct) / 4);
  }

  i2s_stop(VOICE_I2S_PORT);

  if (s_samples < VOICE_SAMPLE_RATE / 10) {   // < 0.1 秒
    fail("录音太短");
  } else {
    s_state = VState::Uploading;
    s_level = 0;
    uploadRecording();
  }

  s_task = nullptr;
  vTaskDelete(nullptr);
}

// ── 对外接口 ──

void voiceInputInit() {
  if (s_initOk) return;

  // 2.16 的麦克风偏置/供电在 ALDO1 上。power.cpp 只在
  // BOARD_AXP_ENABLE_AUX_LDOS 为 1 时统一打开，而这块板该宏是 0，
  // 所以这里单独把 ALDO1 打开，否则 ES7210 采到的是一片静音。
  XPowersPMU* pmu = hwPmuRef();
  if (pmu) {
    pmu->setALDO1Voltage(3300);
    pmu->enableALDO1();
    delay(20);
  }

  audio_hal_codec_config_t cfg = {};
  cfg.adc_input  = AUDIO_HAL_ADC_INPUT_ALL;
  cfg.codec_mode = AUDIO_HAL_CODEC_MODE_ENCODE;
  cfg.i2s_iface.mode    = AUDIO_HAL_MODE_SLAVE;
  cfg.i2s_iface.fmt     = AUDIO_HAL_I2S_NORMAL;
  cfg.i2s_iface.samples = AUDIO_HAL_16K_SAMPLES;
  cfg.i2s_iface.bits    = AUDIO_HAL_BIT_LENGTH_16BITS;

  esp_err_t ret = es7210_adc_init(&Wire, &cfg);
  ret |= es7210_adc_config_i2s(cfg.codec_mode, &cfg.i2s_iface);
  // 30 dB：和 xiaozhi 2.16 参考实现一致；太小会听不清，太大容易削顶。
  ret |= es7210_adc_set_gain_all(GAIN_30DB);
  ret |= es7210_adc_ctrl_state(cfg.codec_mode, AUDIO_HAL_CTRL_START);
  if (ret != ESP_OK) {
    Serial.printf("voice: ES7210 init failed (%d)\n", (int)ret);
  }

  i2s_config_t i2s_cfg = {};
  i2s_cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  i2s_cfg.sample_rate = VOICE_SAMPLE_RATE;
  i2s_cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  i2s_cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  i2s_cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2s_cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  i2s_cfg.dma_buf_count = 8;
  i2s_cfg.dma_buf_len = 320;
  i2s_cfg.use_apll = false;
  i2s_cfg.tx_desc_auto_clear = true;
  i2s_cfg.fixed_mclk = 0;
  i2s_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  i2s_cfg.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;

  i2s_pin_config_t pins = {};
  pins.mck_io_num   = PIN_I2S_MCLK;
  pins.bck_io_num   = PIN_I2S_BCLK;
  pins.ws_io_num    = PIN_I2S_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num  = PIN_I2S_DI;

  esp_err_t i2s_ret = i2s_driver_install(VOICE_I2S_PORT, &i2s_cfg, 0, nullptr);
  if (i2s_ret == ESP_OK) i2s_ret = i2s_set_pin(VOICE_I2S_PORT, &pins);
  if (i2s_ret == ESP_OK) i2s_zero_dma_buffer(VOICE_I2S_PORT);
  if (i2s_ret == ESP_OK) i2s_stop(VOICE_I2S_PORT);   // 按下 BOOT 再启动
  if (i2s_ret != ESP_OK) {
    Serial.printf("voice: I2S init failed (%d)\n", (int)i2s_ret);
  }

  s_pcm = (int16_t*)heap_caps_malloc(VOICE_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_pcm) {
    Serial.printf("voice: PSRAM alloc failed (%u bytes), retry smaller\n",
                  (unsigned)VOICE_MAX_BYTES);
  }

  s_initOk = (ret == ESP_OK) && (i2s_ret == ESP_OK) && (s_pcm != nullptr);
  Serial.printf("voice: init %s, psram=%u, buffer=%u B\n",
                s_initOk ? "OK" : "FAILED",
                (unsigned)ESP.getPsramSize(), (unsigned)VOICE_MAX_BYTES);
}

void voiceInputSetServer(const char* host, uint16_t port, const char* token,
                         bool wifiConnected) {
  const char* h = host ? host : "";
  if (strcmp(s_host, h) != 0) strlcpy(s_host, h, sizeof(s_host));
  s_port = port;
  const char* t = token ? token : "";
  if (strcmp(s_token, t) != 0) strlcpy(s_token, t, sizeof(s_token));
  s_wifi = wifiConnected;
}

static void voiceToggle() {
  if (s_state == VState::Recording) {
    s_stopReq = true;
    Serial.println("voice: stop requested");
    return;
  }
  if (s_state == VState::Uploading) return;   // 识别中忽略

  if (!s_initOk)       { fail("麦克风初始化失败"); return; }
  if (!s_wifi)          { fail("网络未连接");     return; }
  if (!s_host[0] || s_port == 0) { fail("没找到电脑桥接"); return; }
  if (!s_token[0])      { fail("未配置 act_token"); return; }

  strlcpy(s_upHost, s_host, sizeof(s_upHost));
  s_upPort = s_port;
  strlcpy(s_upToken, s_token, sizeof(s_upToken));

  s_samples = 0;
  s_stopReq = false;
  s_level = 0;
  portENTER_CRITICAL(&s_textMux);
  s_text[0] = 0;
  portEXIT_CRITICAL(&s_textMux);
  s_err[0] = 0;

  esp_err_t r = i2s_start(VOICE_I2S_PORT);
  if (r != ESP_OK) {
    fail("麦克风启动失败");
    return;
  }

  s_startedMs = millis();
  s_state = VState::Recording;
  if (xTaskCreatePinnedToCore(voiceTask, "voice_in", 8192, nullptr, 3, &s_task, 0) != pdPASS) {
    s_task = nullptr;
    i2s_stop(VOICE_I2S_PORT);
    fail("录音任务创建失败");
    return;
  }
  Serial.printf("voice: recording -> %s:%u\n", s_upHost, (unsigned)s_upPort);
}

void voiceInputHandleButton(bool allowToggle) {
  if (!allowToggle) return;
  if (!hwBtnBoot().wasPressed) return;
  voiceToggle();
}

void voiceInputTick() {
  // 串口输入 v 等价于按一次 BOOT。这是给调试/自动化用的后备入口：不接
  // 物理按键时也能验证采集和上传链路；正常使用不依赖它。
  if (Serial.available() > 0) {
    int c = Serial.read();
    if (c == 'v' || c == 'V') {
      Serial.println("voice: serial toggle");
      voiceToggle();
    }
  }
  if (s_state == VState::Recording && !s_stopReq &&
      millis() - s_startedMs >= VOICE_MAX_SECONDS * 1000) {
    s_stopReq = true;
    Serial.println("voice: max duration, auto stop");
  }
  if ((s_state == VState::Done || s_state == VState::Error) &&
      s_endedMs != 0 && millis() - s_endedMs >= VOICE_RESULT_MS) {
    s_state = VState::Idle;
    s_endedMs = 0;
    s_redraw = true;
  }
}

bool voiceInputActive() { return s_state != VState::Idle; }

bool voiceInputConsumeRedraw() {
  bool r = s_redraw;
  s_redraw = false;
  return r;
}

void voiceInputDraw() {
  const VState st = s_state;
  spr.fillScreen(C_BG);

  spr.fillCircle(10, 14, 4, C_BLUE);
  spr.setFont(u8g2_font_wqy16_t_gb2312b);
  spr.setTextColor(C_TEXT);
  spr.setCursor(20, 20);
  spr.print("语音输入");

  const char* sub;
  uint16_t subColor;
  if (!s_wifi)            { sub = "网络未连接";      subColor = C_RED; }
  else if (!s_token[0])   { sub = "未配置 act_token"; subColor = C_AMBER; }
  else if (!s_host[0])    { sub = "正在找电脑桥接…";  subColor = C_TEXT2; }
  else                    { sub = "电脑桥接已就绪";    subColor = C_GREEN; }
  spr.setFont(u8g2_font_wqy12_t_gb2312b);
  spr.setTextColor(subColor);
  spr.setCursor(10, 38);
  spr.print(sub);

  char text[192];
  portENTER_CRITICAL(&s_textMux);
  strlcpy(text, s_text, sizeof(text));
  portEXIT_CRITICAL(&s_textMux);

  switch (st) {
    case VState::Recording: {
      if (s_stopReq) {
        drawCentered("正在结束录音", 84, 16, 1, C_AMBER);
        drawCentered("请稍候…", 112, 12, 1, C_TEXT2);
      } else {
        drawCentered("正在录音", 78, 16, 1, C_GREEN);
        uint32_t sec = (millis() - s_startedMs) / 1000;
        char timer[16];
        snprintf(timer, sizeof(timer), "%02u:%02u",
                 (unsigned)(sec / 60), (unsigned)(sec % 60));
        drawCentered(timer, 112, 12, 2, C_TEXT);
        drawLevel(146);
        drawCentered("再按 BOOT 结束", 186, 12, 1, C_TEXT2);
      }
      break;
    }
    case VState::Uploading: {
      static const char* const DOTS[4] = { "", ".", "..", "..." };
      char msg[24];
      snprintf(msg, sizeof(msg), "正在识别%s", DOTS[(millis() / 300) % 4]);
      drawCentered(msg, 96, 16, 1, C_AMBER);
      drawCentered("火山引擎 · 请稍候", 124, 12, 1, C_TEXT2);
      break;
    }
    case VState::Done: {
      drawCentered("已输入到电脑", 62, 16, 1, C_GREEN);
      drawWrapped(text, 10, 88, 164, 17, 5, C_TEXT);
      drawCentered("按 BOOT 再说一句", 190, 12, 1, C_TEXT2);
      break;
    }
    case VState::Error: {
      drawCentered("没有成功", 62, 16, 1, C_RED);
      drawWrapped(s_err, 10, 90, 164, 17, 3, C_TEXT);
      drawCentered("按 BOOT 重试", 190, 12, 1, C_TEXT2);
      break;
    }
    default:
      break;
  }

  char footer[64];
  if (s_host[0]) snprintf(footer, sizeof(footer), "PC %s:%u", s_host, (unsigned)s_port);
  else           snprintf(footer, sizeof(footer), "PC --");
  spr.setFont(u8g2_font_wqy12_t_gb2312b);
  spr.setTextColor(C_TEXT2);
  spr.setCursor(10, 214);
  spr.print(footer);
}
