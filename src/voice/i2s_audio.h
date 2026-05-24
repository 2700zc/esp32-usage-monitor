#pragma once
#include <stdint.h>
#include <stddef.h>

bool audioInit();
bool audioStart();
void audioStop();
size_t audioRead(uint8_t* buf, size_t len);