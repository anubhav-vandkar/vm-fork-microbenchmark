#pragma once
#include "vmbench/backend.hpp"

namespace vmbench {
class LightningBackend final : public ManagedBackend {
public:
    using ManagedBackend::ManagedBackend;
    VmHandle boot() override;
    Object prepare_round(const VmHandle& root, int round) override;
    CloneResult clone(const VmHandle& root, int id) override;
    void destroy(const VmHandle& vm) override;
    void release_round(const VmHandle& root) override;
};
}
