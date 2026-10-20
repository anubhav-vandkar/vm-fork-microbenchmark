#pragma once
#include "vmbench/files.hpp"

namespace vmbench {
Object lightning_request(const fs::path& socket, const Object& request);
}
