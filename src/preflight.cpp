#include "vmbench/preflight.hpp"
#include "vmbench/error.hpp"
#include <sys/utsname.h>
#include <unistd.h>

namespace vmbench {
Object inspect_host(const Options& options) {
    utsname host{};
    require(uname(&host) == 0, "Cannot inspect host");
    return {{"host_kernel", host.release}, {"architecture", host.machine},
        {"kvm_accessible", access("/dev/kvm", R_OK | W_OK) == 0},
        {"lightning_binary_exists", fs::is_regular_file(options.lightning)},
        {"firecracker_binary_exists", fs::is_regular_file(options.firecracker)},
        {"kernel_exists", fs::is_regular_file(options.kernel)}, {"initrd_exists", fs::is_regular_file(options.initrd)},
        {"rounds", options.rounds}, {"forks_per_round", options.per_round},
        {"total_forks", options.rounds * options.per_round}, {"maximum_live_vms", options.per_round + 1},
        {"memory_mib", options.memory_mib}, {"vcpus", options.vcpus}, {"huge_pages", options.huge_pages},
        {"cgroup", options.cgroup.string()}, {"thp_policy", optional_text("/sys/kernel/mm/transparent_hugepage/enabled")}};
}
void validate_runtime(const Options& options, const Object& inspection) {
    require(options.backend == "lightning" || options.backend == "firecracker", "Choose --backend lightning or firecracker");
    require(inspection.at("kvm_accessible").as_bool(), "No accessible /dev/kvm; use a KVM-enabled Linux host");
    require(text(inspection.at("architecture")) == "x86_64", "This checkout's live-fork path is validated on x86_64");
    require(fs::is_regular_file(options.kernel) && fs::is_regular_file(options.initrd), "Supply a compatible --kernel and --initrd");
    require(fs::is_regular_file(options.backend == "lightning" ? options.lightning : options.firecracker), "VMM binary is missing; build it first");
    require(!options.output.empty(), "Supply --output NEW_DIR");
    require(!fs::exists(options.output), "Output directory already exists");
}
}
