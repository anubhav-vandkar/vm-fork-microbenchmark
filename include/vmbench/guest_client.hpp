#pragma once
#include "vmbench/files.hpp"

namespace vmbench {
Object guest_request(const fs::path& socket, const std::string& command = "GET");
}
