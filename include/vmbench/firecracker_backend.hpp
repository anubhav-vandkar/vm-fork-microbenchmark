#pragma once
#include "vmbench/backend.hpp"

namespace vmbench {
class FirecrackerBackend final : public ManagedBackend {
public:
    using ManagedBackend::ManagedBackend;
    VmHandle boot() override;
    Object prepare_round(const VmHandle& root, int round) override;
    CloneResult clone(const VmHandle& root, int id) override;
    void destroy(const VmHandle& vm) override;
    void release_round(const VmHandle& root) override;
private:
    VmHandle start_process(int id);
    void capture_snapshot(const VmHandle& root);
    Object snapshot_metadata(uint64_t elapsed_ns) const;
    void remove_socket_files(const VmHandle& vm) const;
    fs::path state_, memory_;
};
}
