#pragma once
#include "vmbench/file_descriptor.hpp"
#include <filesystem>
#include <memory>
#include <sys/types.h>
#include <vector>

namespace vmbench {
struct Vm {
    int id = 0;
    pid_t pid = -1;
    FileDescriptor pidfd;
    bool alive = true;
    std::filesystem::path control, vsock;
};
using VmHandle = std::shared_ptr<Vm>;
using VmList = std::vector<VmHandle>;
}
