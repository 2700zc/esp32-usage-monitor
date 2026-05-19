#pragma once
#include <stdint.h>

bool usbKeyboardInit();
void usbKeyboardPress(uint8_t key);
void usbKeyboardTypeText(const char* text);
void usbKeyboardEnter();
void usbKeyboardCtrlShiftS();
