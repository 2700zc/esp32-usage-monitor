#include "hw/ble_hid.h"
#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLESecurity.h>
#include <BLEHIDDevice.h>
#include <BLE2902.h>

// 设备名会出现在 Windows 的"添加设备"列表里
static const char* BLE_NAME = "ESP32-Monitor";

static BLEHIDDevice*     s_hid    = nullptr;
static BLECharacteristic* s_input  = nullptr;
static BLECharacteristic* s_output = nullptr;
static volatile bool     s_connected = false;
static volatile bool     s_secure    = false;
static uint32_t          s_nextPrime = 0;

static BLE2902* inputCccd() {
  if (s_input == nullptr) return nullptr;
  return (BLE2902*)s_input->getDescriptorByUUID((uint16_t)0x2902);
}

// Windows 在挂起/重连后不一定重写 CCCD；服务端镜像若还是 false，
// BLECharacteristic::notify() 会直接返回（日志里只到 verbose 级）。
static void forceInputNotifyOn() {
  BLE2902* p = inputCccd();
  if (p != nullptr) p->setNotifications(true);
}

// 标准 8 字节键盘输入报告：修饰键 + 保留 + 6 个按键。
// 右 Alt 是修饰键位 0x40，单独按下就是豆包要的那个键。
// 末尾的 LED 输出报告是 Windows HID 驱动的常规期望（大写锁定等指示灯）。
static const uint8_t REPORT_MAP[] = {
  0x05, 0x01,        // Usage Page (Generic Desktop)
  0x09, 0x06,        // Usage (Keyboard)
  0xA1, 0x01,        // Collection (Application)
  0x85, 0x01,        //   Report ID (1)
  0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
  0x19, 0xE0,        //   Usage Minimum (Left Control)
  0x29, 0xE7,        //   Usage Maximum (Right GUI)
  0x15, 0x00,        //   Logical Minimum (0)
  0x25, 0x01,        //   Logical Maximum (1)
  0x75, 0x01,        //   Report Size (1)
  0x95, 0x08,        //   Report Count (8)
  0x81, 0x02,        //   Input (Data, Variable, Absolute) 修饰键
  0x95, 0x01,        //   Report Count (1)
  0x75, 0x08,        //   Report Size (8)
  0x81, 0x01,        //   Input (Constant) 保留字节
  0x95, 0x06,        //   Report Count (6)
  0x75, 0x08,        //   Report Size (8)
  0x15, 0x00,        //   Logical Minimum (0)
  0x25, 0x65,        //   Logical Maximum (101)
  0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
  0x19, 0x00,        //   Usage Minimum (0)
  0x29, 0x65,        //   Usage Maximum (101)
  0x81, 0x00,        //   Input (Data, Array) 6 个按键
  0x95, 0x05,        //   Report Count (5) LED 输出
  0x75, 0x01,        //   Report Size (1)
  0x05, 0x08,        //   Usage Page (LEDs)
  0x19, 0x01,        //   Usage Minimum (Num Lock)
  0x29, 0x05,        //   Usage Maximum (Kana)
  0x91, 0x02,        //   Output (Data, Variable, Absolute)
  0x95, 0x01,        //   Report Count (1)
  0x75, 0x03,        //   Report Size (3) 补齐 1 字节
  0x91, 0x01,        //   Output (Constant)
  0xC0               // End Collection
};

class BleServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    s_connected = true;
    forceInputNotifyOn();
    s_nextPrime = millis() + 500;   // 连上后先补一个空报告
    Serial.println("ble: connected");
  }
  void onDisconnect(BLEServer* server) override {
    s_connected = false;
    s_secure    = false;
    Serial.println("ble: disconnected, advertising");
    server->startAdvertising();
  }
};

class BleSecurityCB : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override { return 0; }
  void onPassKeyNotify(uint32_t pass_key) override {
    Serial.printf("ble: passkey %06lu\n", (unsigned long)pass_key);
  }
  bool onSecurityRequest() override { return true; }
  void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override {
    s_secure = cmpl.success;
    Serial.printf("ble: auth %s\n", cmpl.success ? "ok" : "failed");
  }
  bool onConfirmPIN(uint32_t) override { return true; }
};

bool bleHidInit() {
  BLEDevice::init(BLE_NAME);
  // 输入报告特征要求加密访问；Just Works（无 PIN）绑定即可满足。
  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
  BLESecurity* sec = new BLESecurity();
  sec->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
  sec->setCapability(ESP_IO_CAP_NONE);
  sec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  BLEDevice::setSecurityCallbacks(new BleSecurityCB());

  BLEServer* server = BLEDevice::createServer();
  if (server == nullptr) return false;
  server->setCallbacks(new BleServerCB());

  s_hid = new BLEHIDDevice(server);
  if (s_hid == nullptr) return false;
  s_hid->manufacturer()->setValue("Espressif");
  s_hid->pnp(0x02, 0x02E5, 0x0001, 0x0100);
  s_hid->hidInfo(0x00, 0x01);
  s_hid->reportMap((uint8_t*)REPORT_MAP, sizeof(REPORT_MAP));
  // 报告特征必须在 startServices() 之前创建，否则不会进入 GATT 服务；
  // Windows 会枚举出 HID 设备但驱动报错、随后断开连接。
  s_input  = s_hid->inputReport(1);
  s_output = s_hid->outputReport(1);
  s_hid->startServices();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->setAppearance(HID_KEYBOARD);
  adv->addServiceUUID(s_hid->hidService()->getUUID());
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);   // Windows 更喜欢这个连接间隔提示
  adv->start();
  Serial.printf("ble: advertising as %s\n", BLE_NAME);
  return true;
}

bool bleHidReady() {
  return s_connected && s_secure && s_input != nullptr;
}

const char* bleHidName() {
  return BLE_NAME;
}

void bleHidTapRightAlt() {
  if (!bleHidReady()) return;
  BLE2902* p = inputCccd();
  const bool sub = p != nullptr && p->getNotifications();
  String pm = s_hid->protocolMode()->getValue();
  const uint8_t proto = pm.length() > 0 ? (uint8_t)pm[0] : 0xFF;
  Serial.printf("ble: tap sub=%d proto=%u\n", sub, proto);
  if (!sub) {
    forceInputNotifyOn();
    Serial.println("ble: force notify on");
  }
  uint8_t report[8] = { 0x40, 0, 0, 0, 0, 0, 0, 0 };  // Right Alt
  s_input->setValue(report, sizeof(report));
  s_input->notify();
  delay(120);
  memset(report, 0, sizeof(report));
  s_input->setValue(report, sizeof(report));
  s_input->notify();
  Serial.println("ble: ralt tap");
}

void bleHidTick() {
  if (!s_connected || s_input == nullptr) return;
  const uint32_t now = millis();
  if ((int32_t)(now - s_nextPrime) < 0) return;
  s_nextPrime = now + 5000;
  uint8_t zero[8] = { 0 };
  s_input->setValue(zero, sizeof(zero));
  s_input->notify();   // 空报告：不产生按键，只保活 HID 通知通道
}
