#pragma once
#include <stdint.h>
#include <Arduino.h>

bool recorderInit();
void recorderStart();
void recorderStop();
bool recorderIsRunning();
const uint8_t* recorderData();
size_t recorderLen();
