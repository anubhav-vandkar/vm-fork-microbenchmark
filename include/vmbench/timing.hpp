#pragma once
#include <chrono>
#include <cstdint>

namespace vmbench {
using Clock = std::chrono::steady_clock;
uint64_t now_ns();
void sleep_ms(int milliseconds);
}
