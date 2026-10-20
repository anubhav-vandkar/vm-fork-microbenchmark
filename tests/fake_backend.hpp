#pragma once
#include "vmbench/backend.hpp"

namespace vmbench::tests {
class FakeBackend final : public Backend {
public:
    explicit FakeBackend(int batch_size) : batch_size_(batch_size) {}
    VmHandle boot() override;
    void marker(const VmHandle& root, int round) override;
    Object ready(const VmHandle& vm) override;
    Object prepare_round(const VmHandle& root, int round) override;
    CloneResult clone(const VmHandle& root, int id) override;
    void destroy(const VmHandle& vm) override;
    void release_round(const VmHandle& root) override;
    json::array cleanup() override { return {}; }
    VmList nodes;
    bool corrupt_child = false;
private:
    int batch_size_, marker_ = 0, created_ = 0;
    bool prepared_ = false;
};
}
