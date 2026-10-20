#include "vmbench/lightning_backend.hpp"
#include "vmbench/error.hpp"
#include "vmbench/guest_config.hpp"
#include "vmbench/lightning_client.hpp"
#include "vmbench/timing.hpp"

namespace vmbench {
VmHandle LightningBackend::boot() {
    const auto control = work_ / "root.sock";
    const auto vsock = work_ / "root-v.sock";
    const auto config_path = work_ / "config.json";
    auto configuration = guest_configuration(options_, vsock);
    configuration["drives"] = json::array{};
    write_text(config_path, json::serialize(configuration));
    const auto pid = launch({options_.lightning.string(), "live", config_path.string(), control.string(),
        (work_ / "children").string(), "--no-seccomp"}, "lightning-root");
    processes_.wait_for_socket(control, pid);
    auto root = processes_.track(0, pid, control, vsock);
    ready(root);
    return root;
}
Object LightningBackend::prepare_round(const VmHandle&, int) { return {{"mechanism", "live fork"}}; }
CloneResult LightningBackend::clone(const VmHandle& root, int id) {
    auto response = lightning_request(root->control, {{"op", "fork"}});
    auto child = processes_.track(id, static_cast<pid_t>(integer(response.at("pid"))),
        text(response.at("control_path")), text(response.at("vsock_path")));
    return {child, std::move(response)};
}
void LightningBackend::destroy(const VmHandle& vm) {
    if (!vm->alive) return;
    try { lightning_request(vm->control, {{"op", "shutdown"}}); }
    catch (const std::exception&) { /* ProcessManager escalates if graceful shutdown failed. */ }
    processes_.stop(vm, 0);
}
void LightningBackend::release_round(const VmHandle& root) {
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (!lightning_request(root->control, {{"op", "status"}}).at("children").as_array().empty()) {
        require(Clock::now() < deadline, "Parent has unreaped children");
        sleep_ms(10);
    }
}
}
