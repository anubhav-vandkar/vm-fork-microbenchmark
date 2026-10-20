#pragma once
#include <stdexcept>
#include <string>

namespace vmbench {
inline void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
}
