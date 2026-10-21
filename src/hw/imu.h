#pragma once

bool hwImuInit();
// 读取失败返回 false，输出保持不变；对失败敏感的调用方用这个。
bool hwImuReadAccel(float* ax, float* ay, float* az);
bool hwImuReadGyro(float* gx, float* gy, float* gz);
// 兼容旧接口：失败时输出 0。
void hwImuAccel(float* ax, float* ay, float* az);
