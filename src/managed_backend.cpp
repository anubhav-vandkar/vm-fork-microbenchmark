#include "vmbench/backend.hpp"
#include "vmbench/guest_client.hpp"

namespace vmbench {
ManagedBackend::ManagedBackend(const Options& options, fs::path work) : options_(options), work_(std::move(work)) {}
void ManagedBackend::marker(const VmHandle& root, int round) {
    guest_request(root->vsock, "SET " + std::to_string(round));
}
Object ManagedBackend::ready(const VmHandle& vm) { return guest_request(vm->vsock); }
pid_t ManagedBackend::launch(const std::vector<std::string>& arguments, const std::string& log_label) {
    return processes_.launch(arguments, options_.output / (log_label + ".log"));
}
json::array ManagedBackend::cleanup() {
    json::array errors;
    const auto active = processes_.active();
    for (auto it = active.rbegin(); it != active.rend(); ++it) {
        try { destroy(*it); }
        catch (const std::exception& error) { errors.push_back(json::value(error.what())); }
    }
    for (auto& error : processes_.terminate_remaining()) errors.push_back(std::move(error));
    return errors;
}
}
