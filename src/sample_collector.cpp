#include "vmbench/sample_collector.hpp"
#include "vmbench/cgroup_reader.hpp"
#include "vmbench/kvm_reader.hpp"
#include "vmbench/proc_reader.hpp"
#include "vmbench/timing.hpp"

namespace vmbench {
Object SampleCollector::collect(const VmList& vms, bool include_kvm_counters) const {
    json::array processes;
    for (const auto& vm : vms) processes.push_back(read_process_metrics(*vm));
    Object result{{"monotonic_ns", now_ns()}, {"live_vms", vms.size()}, {"memory_sum_kib", sum_process_memory(processes)}};
    result["processes"] = std::move(processes);
    result["cgroup"] = cgroup_.empty() ? json::value(nullptr) : json::value(read_cgroup_metrics(cgroup_));
    if (include_kvm_counters) result["kvm_debugfs"] = read_kvm_metrics(vms);
    return result;
}
}
