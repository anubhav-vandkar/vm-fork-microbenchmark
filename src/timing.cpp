#include "vmbench/timing.hpp"
#include <thread>

namespace vmbench {
uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
void sleep_ms(int milliseconds) {
    if (milliseconds > 0) std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}
}
