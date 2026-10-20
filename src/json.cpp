#include <boost/json/src.hpp>
#include "vmbench/json.hpp"
#include <limits>
#include <stdexcept>

namespace vmbench {
int64_t integer(const json::value& value) {
    if (value.is_int64()) return value.as_int64();
    if (value.is_uint64() && value.as_uint64() <= uint64_t(std::numeric_limits<int64_t>::max())) {
        return static_cast<int64_t>(value.as_uint64());
    }
    throw std::runtime_error("Expected JSON integer within signed 64-bit range");
}
std::string text(const json::value& value) {
    return std::string(value.as_string());
}
}
