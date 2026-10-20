#include "vmbench/kvm_reader.hpp"

namespace vmbench {
namespace {
Object read_vm_counters(const fs::path& directory) {
    Object counters;
    for (const auto* name : {"pages_4k", "pages_2m", "pages_1g", "mmu_cache_miss", "mmu_shadow_zapped", "pf_taken", "pf_fixed", "exits"}) {
        try { counters[name] = std::stoull(read_text(directory / name)); }
        catch (const std::exception&) { /* Optional counters vary by kernel and access. */ }
    }
    return counters;
}
std::vector<fs::path> vm_directories(const fs::path& directory) {
    std::vector<fs::path> result;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (entry.is_directory()) result.push_back(entry.path());
    }
    return result;
}
}
Object read_kvm_metrics(const VmList& vms, const fs::path& directory) {
    Object result;
    try {
        const auto directories = vm_directories(directory);
        for (const auto& vm : vms) {
            Object matches;
            const auto prefix = std::to_string(vm->pid) + "-";
            for (const auto& candidate : directories) {
                if (candidate.filename().string().starts_with(prefix)) matches[candidate.filename().string()] = read_vm_counters(candidate);
            }
            result[std::to_string(vm->pid)] = std::move(matches);
        }
    } catch (const std::exception& error) { result["unavailable"] = error.what(); }
    return result;
}
}
