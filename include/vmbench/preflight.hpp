#pragma once
#include "vmbench/options.hpp"

namespace vmbench {
Object inspect_host(const Options& options);
void validate_runtime(const Options& options, const Object& inspection);
}
