#pragma once
#include "vmbench/files.hpp"
#include "vmbench/vm.hpp"

namespace vmbench {
class SampleCollector {
public:
    explicit SampleCollector(fs::path cgroup) : cgroup_(std::move(cgroup)) {}
    Object collect(const VmList& vms, bool include_kvm_counters) const;
private:
    fs::path cgroup_;
};
}
