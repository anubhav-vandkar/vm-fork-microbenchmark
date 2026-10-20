#include "vmbench/process_manager.hpp"
#include "vmbench/error.hpp"
#include "vmbench/signals.hpp"
#include "vmbench/timing.hpp"
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/syscall.h>
#include <sys/wait.h>

namespace vmbench {
namespace {
FileDescriptor open_pidfd(pid_t pid) {
    const int descriptor = static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
    require(descriptor >= 0, "pidfd_open failed");
    return FileDescriptor(descriptor);
}
bool exited(int descriptor, int timeout_ms = 0) {
    pollfd event{descriptor, POLLIN, 0};
    int result;
    do { result = poll(&event, 1, timeout_ms); } while (result < 0 && errno == EINTR);
    require(result >= 0, "pidfd poll failed");
    return result > 0;
}
void signal_process(int descriptor, int signal) {
    const auto result = syscall(SYS_pidfd_send_signal, descriptor, signal, nullptr, 0);
    require(result == 0 || errno == ESRCH, "pidfd signal failed");
}
void reap_if_owned(pid_t pid) {
    int status;
    while (waitpid(pid, &status, WNOHANG) < 0 && errno == EINTR) {}
}
void await_exit(int descriptor) {
    if (exited(descriptor, 5000)) return;
    signal_process(descriptor, SIGKILL);
    require(exited(descriptor, 5000), "VMM did not exit");
}
void redirect_child_io(const fs::path& log) {
    const int input = open("/dev/null", O_RDONLY);
    const int output = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (input < 0 || output < 0) _exit(126);
    if (dup2(input, 0) < 0 || dup2(output, 1) < 0 || dup2(output, 2) < 0) _exit(126);
    ::close(input);
    ::close(output);
}
pid_t spawn(const std::vector<std::string>& arguments, const fs::path& log) {
    require(!arguments.empty(), "Cannot launch an empty command");
    std::vector<char*> argv;
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    const auto pid = fork();
    require(pid >= 0, "Controller process fork failed");
    if (pid == 0) {
        redirect_child_io(log);
        execv(argv.front(), argv.data());
        _exit(127);
    }
    return pid;
}
}
pid_t ProcessManager::launch(const std::vector<std::string>& arguments, const fs::path& log) {
    const auto pid = spawn(arguments, log);
    try {
        launched_.emplace(pid, open_pidfd(pid));
    } catch (...) {
        // No wait/reap has occurred, so this is still our own unreaped child.
        kill(pid, SIGKILL);
        int status;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        throw;
    }
    return pid;
}
VmHandle ProcessManager::track(int id, pid_t pid, const fs::path& control, const fs::path& vsock) {
    auto vm = std::make_shared<Vm>();
    vm->id = id;
    vm->pid = pid;
    vm->control = control;
    vm->vsock = vsock;
    vm->pidfd = open_pidfd(pid);
    active_.push_back(vm);
    return vm;
}
void ProcessManager::wait_for_socket(const fs::path& path, pid_t pid) const {
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    while (!fs::exists(path)) {
        check_interrupted();
        int status;
        require(waitpid(pid, &status, WNOHANG) == 0, "VMM exited before socket became available");
        require(Clock::now() < deadline, "Timed out waiting for " + path.string());
        sleep_ms(2);
    }
}
void ProcessManager::forget(const VmHandle& vm) {
    launched_.erase(vm->pid);
    vm->pidfd.reset();
    vm->alive = false;
    active_.erase(std::remove(active_.begin(), active_.end(), vm), active_.end());
}
void ProcessManager::stop(const VmHandle& vm, int initial_signal) {
    if (!vm->alive) return;
    if (!exited(vm->pidfd.get())) {
        if (initial_signal != 0) signal_process(vm->pidfd.get(), initial_signal);
        await_exit(vm->pidfd.get());
    }
    reap_if_owned(vm->pid);
    forget(vm);
}
json::array ProcessManager::terminate_remaining() noexcept {
    json::array errors;
    const auto copy = active_;
    for (auto it = copy.rbegin(); it != copy.rend(); ++it) {
        try { stop(*it, SIGKILL); }
        catch (const std::exception& error) { errors.push_back(json::value(error.what())); }
    }
    for (const auto& [pid, descriptor] : launched_) {
        try {
            if (!exited(descriptor.get())) {
                signal_process(descriptor.get(), SIGKILL);
                await_exit(descriptor.get());
            }
        } catch (const std::exception& error) { errors.push_back(json::value(error.what())); }
        reap_if_owned(pid);
    }
    launched_.clear();
    return errors;
}
ProcessManager::~ProcessManager() { terminate_remaining(); }
}
