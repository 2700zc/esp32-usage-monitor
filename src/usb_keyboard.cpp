#include "usb_keyboard.h"
#include <Arduino.h>
#include "USB.h"
#include "USBHIDKeyboard.h"

static USBHIDKeyboard s_kbd;

bool usbKeyboardInit() {
  s_kbd.begin();
  USB.begin();
  return true;
}

void usbKeyboardPress(uint8_t key) {
  s_kbd.press(key);
  delay(10);
  s_kbd.release(key);
}

void usbKeyboardTypeText(const char* text) {
  if (text) s_kbd.print(text);
}

void usbKeyboardEnter() {
  usbKeyboardPress(KEY_RETURN);
}

void usbKeyboardCtrlShiftS() {
  s_kbd.press(KEY_LEFT_CTRL);
  s_kbd.press(KEY_LEFT_SHIFT);
  s_kbd.press('s');
  delay(10);
  s_kbd.releaseAll();
}
