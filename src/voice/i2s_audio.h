#pragma once
#include <stdint.h>
#include <stddef.h>

bool audioInit();
bool audioTxStart();
void audioTxWrite(const uint8_t* data, size_t len);
void audioTxStop();
