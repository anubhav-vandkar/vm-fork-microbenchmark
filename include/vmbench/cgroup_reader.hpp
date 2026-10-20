#pragma once
#include "vmbench/files.hpp"

namespace vmbench {
fs::path find_cgroup();
Object select_memory_categories(const Object& statistics);
Object read_cgroup_metrics(const fs::path& directory);
}
