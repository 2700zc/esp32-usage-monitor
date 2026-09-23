#pragma once
#include <stdint.h>

enum class SwipeDir : uint8_t { None, Left, Right };

// 在主循环里 hwInputUpdate() 之后每帧调用一次；返回本帧结算出的滑动方向。
// 输入层只给每帧的 down/x/y 与按下/抬起边沿，没有历史，所以这里自己攒状态。
SwipeDir hwSwipePoll();
