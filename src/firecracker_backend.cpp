#include "vmbench/firecracker_backend.hpp"
#include "vmbench/error.hpp"
#include "vmbench/firecracker_client.hpp"
#include "vmbench/guest_config.hpp"
#include "vmbench/timing.hpp"
#include <csignal>
#include <sys/stat.h>

namespace vmbench {
VmHandle FirecrackerBackend::start_process(int id) {
    const auto suffix = std::to_string(id);
    const auto control = work_ / ("f" + suffix + ".sock");
    const auto pid = launch({options_.firecracker.string(), "--api-sock", control.string(), "--no-seccomp"}, "fc-" + suffix);
    processes_.wait_for_socket(control, pid);
    return processes_.track(id, pid, control, work_ / ("v" + suffix + ".sock"));
}
VmHandle FirecrackerBackend::boot() {
    auto root = start_process(0);
    for (const auto& item : guest_configuration(options_, root->vsock)) {
        firecracker_request(root->control, "PUT", "/" + std::string(item.key()), item.value().as_object());
    }
    firecracker_request(root->control, "PUT", "/actions", {{"action_type", "InstanceStart"}});
    ready(root);
    return root;
}
void FirecrackerBackend::capture_snapshot(const VmHandle& root) {
    firecracker_request(root->control, "PATCH", "/vm", {{"state", "Paused"}});
    firecracker_request(root->control, "PUT", "/snapshot/create", {
        {"snapshot_type", "Full"}, {"snapshot_path", state_.string()},
        {"mem_file_path", memory_.string()}, {"sync_snapshot_files", true}});
    firecracker_request(root->control, "PATCH", "/vm", {{"state", "Resumed"}});
}
Object FirecrackerBackend::snapshot_metadata(uint64_t elapsed_ns) const {
    struct stat metadata{};
    require(stat(memory_.c_str(), &metadata) == 0, "Cannot stat snapshot");
    return {{"mechanism", "one shared full snapshot per round"}, {"prepare_ns", elapsed_ns},
        {"snapshot_state_bytes", fs::file_size(state_)}, {"snapshot_memory_bytes", static_cast<uint64_t>(metadata.st_size)},
        {"snapshot_allocated_bytes", static_cast<uint64_t>(metadata.st_blocks) * 512}, {"sync_snapshot_files", true}};
}
Object FirecrackerBackend::prepare_round(const VmHandle& root, int round) {
    state_ = work_ / ("r" + std::to_string(round) + ".state");
    memory_ = work_ / ("r" + std::to_string(round) + ".mem");
    const auto started = now_ns();
    capture_snapshot(root);
    return snapshot_metadata(now_ns() - started);
}
CloneResult FirecrackerBackend::clone(const VmHandle&, int id) {
    auto child = start_process(id);
    firecracker_request(child->control, "PUT", "/snapshot/load", {{"snapshot_path", state_.string()},
        {"mem_backend", Object{{"backend_type", "File"}, {"backend_path", memory_.string()}}},
        {"vsock_override", Object{{"uds_path", child->vsock.string()}}}, {"resume_vm", true}});
    return {child, Object{{"mechanism", "restore shared round snapshot"}}};
}
void FirecrackerBackend::remove_socket_files(const VmHandle& vm) const {
    fs::remove(vm->control);
    fs::remove(vm->vsock);
    const auto prefix = vm->vsock.filename().string() + "_";
    for (const auto& file : fs::directory_iterator(work_)) {
        if (file.path().filename().string().starts_with(prefix)) fs::remove(file.path());
    }
}
void FirecrackerBackend::destroy(const VmHandle& vm) {
    if (!vm->alive) return;
    processes_.stop(vm, SIGTERM);
    remove_socket_files(vm);
}
void FirecrackerBackend::release_round(const VmHandle&) {
    if (!state_.empty()) fs::remove(state_);
    if (!memory_.empty()) fs::remove(memory_);
    state_.clear();
    memory_.clear();
}
}
