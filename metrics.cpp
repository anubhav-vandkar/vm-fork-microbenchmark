#include "benchmark.hpp"
#include <fstream>
#include <iterator>
#include <sstream>



namespace vmbench {
std::string read_text(const fs::path& path) {
    std::ifstream input(path);
    require(bool(input), "Cannot read " + path.string());
    return {std::istreambuf_iterator<char>(input), {}};
}
json::value optional_text(const fs::path& path) {
    try {
        return json::value(read_text(path));
    } catch (const std::exception& error) {
        return Object{{"unavailable", error.what()}};
    }
}
}


namespace vmbench {
namespace {
Object read_descriptors(const fs::path& directory) {
    Object kvm_descriptors;
    uint64_t total = 0, vm_count = 0, vcpu_alias_count = 0;
    for (const auto& descriptor : fs::directory_iterator(directory)) {
        std::error_code error;
        const auto target = fs::read_symlink(descriptor.path(), error).string();
        if (error) continue;
        ++total;
        if (target.find("kvm") != std::string::npos) kvm_descriptors[descriptor.path().filename().string()] = target;
        vm_count += target.find("kvm-vm") != std::string::npos;
        vcpu_alias_count += target.find("kvm-vcpu") != std::string::npos;
    }
    return {{"fd_count", total}, {"kvm_vm_fd_count", vm_count},
        {"kvm_vcpu_fd_alias_count", vcpu_alias_count}, {"kvm_fds", std::move(kvm_descriptors)}};
}
}
Object parse_numeric_lines(const std::string& input, bool colon_delimited) {
    Object result;
    std::istringstream lines(input);
    std::string line;
    while (std::getline(lines, line)) {
        if (colon_delimited) {
            const auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            line[colon] = ' ';
        }
        std::istringstream fields(line);
        std::string key;
        uint64_t value;
        if (fields >> key >> value) result[key] = value;
    }
    return result;
}
Object read_process_metrics(const Vm& vm) {
    const auto directory = fs::path("/proc") / std::to_string(vm.pid);
    const auto status = parse_numeric_lines(read_text(directory / "status"), true);
    auto result = read_descriptors(directory / "fd");
    result["vm_id"] = vm.id;
    result["pid"] = vm.pid;
    result["threads"] = status.at("Threads");
    result["memory_kib"] = parse_numeric_lines(read_text(directory / "smaps_rollup"), true);
    return result;
}
Object sum_process_memory(const json::array& processes) {
    Object totals{{"Pss", 0}, {"Rss", 0}, {"Private_Dirty", 0}, {"Private_Clean", 0}, {"SwapPss", 0}};
    for (const auto& process : processes) {
        const auto& memory = process.as_object().at("memory_kib").as_object();
        for (auto& total : totals) {
            if (const auto value = memory.if_contains(total.key())) total.value() = integer(total.value()) + integer(*value);
        }
    }
    return totals;
}
}

namespace vmbench {
fs::path find_cgroup() {
    std::istringstream lines(read_text("/proc/self/cgroup"));
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.starts_with("0::")) continue;
        const auto path = fs::path("/sys/fs/cgroup") / fs::path(line.substr(3)).relative_path();
        if (fs::exists(path / "memory.current")) return path;
    }
    return {};
}
Object select_memory_categories(const Object& statistics) {
    Object result;
    for (const auto* category : {"anon", "file", "shmem", "sec_pagetables"}) {
        if (const auto value = statistics.if_contains(category)) result[category] = *value;
    }
    return result;
}
Object read_cgroup_metrics(const fs::path& directory) {
    Object result;
    for (const auto* file : {"memory.current", "memory.peak", "memory.swap.current", "memory.events", "cpu.stat", "pids.current", "io.stat"}) {
        result[file] = optional_text(directory / file);
    }
    const auto memory = optional_text(directory / "memory.stat");
    result["memory.stat"] = memory.is_string()
        ? select_memory_categories(parse_numeric_lines(text(memory), false))
        : Object{{"unavailable", memory}};
    return result;
}
}

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
