#pragma once
#include <stdint.h>
#include <stddef.h>

bool audioInit();
bool audioTxStart();
void audioTxWrite(const uint8_t* data, size_t len);
void audioTxStop();

bool audioRxStart();
size_t audioRxRead(uint8_t* buf, size_t maxLen);
void audioRxStop();
