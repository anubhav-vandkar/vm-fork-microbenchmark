#pragma once
#include "vmbench/files.hpp"
#include "vmbench/vm.hpp"
#include <map>

namespace vmbench {
class ProcessManager {
public:
    pid_t launch(const std::vector<std::string>& arguments, const fs::path& log);
    VmHandle track(int id, pid_t pid, const fs::path& control, const fs::path& vsock);
    void wait_for_socket(const fs::path& path, pid_t pid) const;
    void stop(const VmHandle& vm, int initial_signal);
    json::array terminate_remaining() noexcept;
    const VmList& active() const { return active_; }
    size_t launched_count() const { return launched_.size(); }
    ~ProcessManager();
private:
    void forget(const VmHandle& vm);
    VmList active_;
    std::map<pid_t, FileDescriptor> launched_;
};
}
