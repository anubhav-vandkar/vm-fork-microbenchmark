#include "vmbench/cgroup_reader.hpp"
#include "vmbench/proc_reader.hpp"
#include <sstream>

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
