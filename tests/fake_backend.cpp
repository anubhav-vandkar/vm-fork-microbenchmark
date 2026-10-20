#include "fake_backend.hpp"
#include "vmbench/error.hpp"
#include <algorithm>

namespace vmbench::tests {
VmHandle FakeBackend::boot() {
    auto root = std::make_shared<Vm>();
    root->pid = 10000;
    nodes.push_back(root);
    return root;
}
void FakeBackend::marker(const VmHandle& root, int round) {
    require(root == nodes.front() && root->alive, "Wrong or dead source");
    marker_ = round;
}
Object FakeBackend::ready(const VmHandle& vm) {
    require(vm->alive, "Dead VM used");
    return {{"counter", corrupt_child && vm != nodes.front() ? -1 : marker_}, {"allocated", 0}};
}
Object FakeBackend::prepare_round(const VmHandle&, int) {
    require(!prepared_, "Overlapping rounds");
    prepared_ = true;
    created_ = 0;
    return {};
}
CloneResult FakeBackend::clone(const VmHandle& root, int id) {
    require(root == nodes.front() && prepared_, "Child was not forked from persistent parent");
    auto vm = std::make_shared<Vm>();
    vm->id = id;
    vm->pid = 10000 + id;
    nodes.push_back(vm);
    ++created_;
    return {vm, {}};
}
void FakeBackend::destroy(const VmHandle& vm) {
    require(created_ == batch_size_, "Destroyed a child before full batch measurement");
    require(vm != nodes.front(), "Destroyed persistent parent");
    vm->alive = false;
}
void FakeBackend::release_round(const VmHandle&) {
    const auto alive = std::count_if(nodes.begin(), nodes.end(), [](const auto& vm) { return vm->alive; });
    require(alive == 1, "Children remain alive after batch cleanup");
    prepared_ = false;
}
}
