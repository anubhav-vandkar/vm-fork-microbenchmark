#include "vmbench/guest_config.hpp"

namespace vmbench {
Object guest_configuration(const Options& options, const fs::path& vsock) {
    return {
        {"boot-source", Object{{"kernel_image_path", options.kernel.string()}, {"initrd_path", options.initrd.string()},
            {"boot_args", "console=ttyS0 reboot=k panic=1 pci=off rdinit=/init loglevel=5"}}},
        {"machine-config", Object{{"vcpu_count", options.vcpus}, {"mem_size_mib", options.memory_mib}, {"huge_pages", options.huge_pages}}},
        {"vsock", Object{{"guest_cid", 3}, {"uds_path", vsock.string()}}}
    };
}
}
