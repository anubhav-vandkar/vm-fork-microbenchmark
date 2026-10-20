#pragma once
#include "vmbench/options.hpp"
namespace vmbench {
Object guest_configuration(const Options& options, const fs::path& vsock);
}
