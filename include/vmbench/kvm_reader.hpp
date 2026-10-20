#pragma once
#include "vmbench/files.hpp"
#include "vmbench/vm.hpp"

namespace vmbench {
Object read_kvm_metrics(const VmList& vms, const fs::path& directory = "/sys/kernel/debug/kvm");
}
