#include "vmbench/proc_reader.hpp"
#include <sstream>

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
