#include "benchmark.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <linux/fs.h>
#include <poll.h>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <vector>



namespace vmbench {
class ManagedBackend : public Backend {
public:
    ManagedBackend(const Options& options, fs::path work);
    void marker(const VmHandle& root, int round) override;
    Object ready(const VmHandle& vm) override;
    json::array cleanup() override;
protected:
    pid_t launch(const std::vector<std::string>& arguments, const std::string& log_label);
    const Options& options_;
    fs::path work_;
    ProcessManager processes_;
};

class LightningBackend final : public ManagedBackend {
public:
    using ManagedBackend::ManagedBackend;
    VmHandle boot() override;
    Object prepare_round(const VmHandle& root, int round) override;
    CloneResult clone(const VmHandle& root, int id) override;
    void destroy(const VmHandle& vm) override;
    void release_round(const VmHandle& root) override;
};
class FirecrackerBackend final : public ManagedBackend {
public:
    using ManagedBackend::ManagedBackend;
    VmHandle boot() override;
    Object prepare_round(const VmHandle& root, int round) override;
    CloneResult clone(const VmHandle& root, int id) override;
    void destroy(const VmHandle& vm) override;
    void release_round(const VmHandle& root) override;
    json::array cleanup() override;
private:
    struct SnapshotFiles { fs::path state, memory; };
    VmHandle start_process(int id);
    void capture_snapshot(const VmHandle& root, const SnapshotFiles& files);
    Object prepare_snapshot(const VmHandle& root, const SnapshotFiles& files);
    Object snapshot_metadata(const SnapshotFiles& files, uint64_t elapsed_ns) const;
    void remove_snapshot_files(const SnapshotFiles& files) const;
    void remove_socket_files(const VmHandle& vm) const;
    std::map<int, SnapshotFiles> snapshots_;
    fs::path base_;
};
Object guest_configuration(const Options& options, const fs::path& vsock);
}


namespace vmbench {
namespace {
volatile sig_atomic_t interrupted = 0;
void on_signal(int) { interrupted = 1; }
}
void install_signal_handlers() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);
}
void check_interrupted() {
    if (interrupted) throw std::runtime_error("Interrupted");
}
}

namespace vmbench {
uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
void sleep_ms(int milliseconds) {
    if (milliseconds > 0) std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}
}

namespace vmbench {
TemporaryDirectory::TemporaryDirectory(const std::filesystem::path& parent) {
    const auto pattern = (parent / "v-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    const char* created = mkdtemp(name.data());
    require(created != nullptr, "Cannot create temporary directory");
    path_ = created;
}
TemporaryDirectory::~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
}
}

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

namespace vmbench {
Object guest_configuration(const Options& options, const fs::path& vsock) {
    return {
        {"boot-source", Object{{"kernel_image_path", options.kernel.string()}, {"initrd_path", options.initrd.string()},
            {"boot_args", "console=ttyS0 reboot=k panic=1 pci=off rdinit=/init loglevel=5"}}},
        {"machine-config", Object{{"vcpu_count", options.vcpus}, {"mem_size_mib", options.memory_mib}, {"huge_pages", options.huge_pages}}},
        {"vsock", Object{{"guest_cid", 3}, {"uds_path", vsock.string()}}}
    };
}
}

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

namespace vmbench {
namespace {
FileDescriptor open_snapshot_file(const fs::path& path, int flags) {
    FileDescriptor descriptor(open(path.c_str(), flags | O_CLOEXEC, 0600));
    require(descriptor.get() >= 0, "Cannot open snapshot memory: " + path.string());
    return descriptor;
}
off_t snapshot_length(int descriptor) {
    struct stat metadata{};
    require(fstat(descriptor, &metadata) == 0 && metadata.st_size > 0, "Invalid snapshot memory size");
    return metadata.st_size;
}
void sync_snapshot(int descriptor) {
    require(fsync(descriptor) == 0, "Cannot synchronize snapshot memory");
}
void copy_extent(int source, int destination, off_t begin, off_t end) {
    std::array<char, 65536> buffer;
    while (begin < end) {
        check_interrupted();
        ssize_t count;
        do { count = pread(source, buffer.data(), std::min<off_t>(buffer.size(), end - begin), begin); }
        while (count < 0 && errno == EINTR);
        require(count > 0, "Cannot read snapshot extent");
        for (ssize_t written = 0; written < count;) {
            ssize_t result;
            do { result = pwrite(destination, buffer.data() + written, count - written, begin + written); }
            while (result < 0 && errno == EINTR);
            require(result > 0, "Cannot write snapshot extent");
            written += result;
        }
        begin += count;
    }
}
void copy_data_extents(int source, int destination, off_t length) {
    for (off_t position = 0; position < length;) {
        const auto data = lseek(source, position, SEEK_DATA);
        if (data < 0 && errno == ENXIO) break;
        require(data >= position && data < length, "Snapshot filesystem must support SEEK_DATA");
        const auto hole = lseek(source, data, SEEK_HOLE);
        require(hole > data && hole <= length, "Snapshot filesystem must support SEEK_HOLE");
        copy_extent(source, destination, data, hole);
        position = hole;
    }
}
}

std::string copy_snapshot_memory(const fs::path& source, const fs::path& destination, bool allow_reflink) {
    auto input = open_snapshot_file(source, O_RDONLY);
    auto output = open_snapshot_file(destination, O_RDWR | O_CREAT | O_EXCL);
    const auto length = snapshot_length(input.get());
    if (allow_reflink && ioctl(output.get(), FICLONE, input.get()) == 0) {
        sync_snapshot(output.get());
        return "reflink";
    }
    if (allow_reflink) require(errno == EOPNOTSUPP || errno == ENOTTY || errno == EXDEV || errno == EINVAL || errno == ENOSYS,
        "Snapshot reflink failed (not an unsupported-filesystem error)");
    require(ftruncate(output.get(), 0) == 0 && ftruncate(output.get(), length) == 0, "Cannot size sparse child memory");
    copy_data_extents(input.get(), output.get(), length);
    sync_snapshot(output.get());
    return "sparse-copy";
}
}

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
    auto configuration = guest_configuration(options_, root->vsock);
    configuration["machine-config"].as_object()["track_dirty_pages"] = options_.snapshot_type == "diff";
    for (const auto& item : configuration) {
        firecracker_request(root->control, "PUT", "/" + std::string(item.key()), item.value().as_object());
    }
    firecracker_request(root->control, "PUT", "/actions", {{"action_type", "InstanceStart"}});
    ready(root);
    return root;
}
void FirecrackerBackend::capture_snapshot(const VmHandle& root, const SnapshotFiles& files) {
    firecracker_request(root->control, "PATCH", "/vm", {{"state", "Paused"}});
    firecracker_request(root->control, "PUT", "/snapshot/create", {
        {"snapshot_type", options_.snapshot_type == "diff" ? "Diff" : "Full"}, {"snapshot_path", files.state.string()},
        {"mem_file_path", (options_.snapshot_type == "diff" ? base_ : files.memory).string()}, {"sync_snapshot_files", true}});
    firecracker_request(root->control, "PATCH", "/vm", {{"state", "Resumed"}});
}
Object FirecrackerBackend::snapshot_metadata(const SnapshotFiles& files, uint64_t elapsed_ns) const {
    struct stat metadata{};
    require(stat(files.memory.c_str(), &metadata) == 0, "Cannot stat snapshot");
    return {{"mechanism", options_.snapshot_type == "diff" ? "fresh KVM dirty-page diff and materialized restore" : "fresh full snapshot and restore"},
        {"snapshot_type", options_.snapshot_type}, {"track_dirty_pages", options_.snapshot_type == "diff"},
        {"child_track_dirty_pages", false}, {"snapshot_policy", "per-fork"},
        {"preparation_included_in_guest_ready", true}, {"prepare_ns", elapsed_ns},
        {"snapshot_path", files.state.string()}, {"snapshot_memory_path", files.memory.string()},
        {"snapshot_state_bytes", fs::file_size(files.state)}, {"snapshot_memory_bytes", static_cast<uint64_t>(metadata.st_size)},
        {"snapshot_allocated_bytes", static_cast<uint64_t>(metadata.st_blocks) * 512}, {"sync_snapshot_files", true}};
}
Object FirecrackerBackend::prepare_snapshot(const VmHandle& root, const SnapshotFiles& files) {
    const auto started = now_ns();
    const bool initial = base_.empty();
    if (options_.snapshot_type == "diff" && initial) base_ = work_ / "base.mem";
    // The base is never loaded by a child. Firecracker updates it in-place with
    // dirty pages; its first diff from a booted VM supplies all nonzero history.
    capture_snapshot(root, files);
    const auto captured = now_ns();
    Object extra{{"capture_ns", captured - started}};
    if (options_.snapshot_type == "diff") {
        extra["snapshot_copy_method"] = copy_snapshot_memory(base_, files.memory);
        extra["backing_copy_ns"] = now_ns() - captured;
        extra["merge_in_capture"] = true;
        extra["initial_base"] = initial;
        extra["base_memory_path"] = base_.string();
        struct stat base{};
        require(stat(base_.c_str(), &base) == 0, "Cannot stat cumulative base");
        extra["base_memory_bytes"] = static_cast<uint64_t>(base.st_size);
        extra["base_allocated_bytes"] = static_cast<uint64_t>(base.st_blocks) * 512;
    }
    auto metadata = snapshot_metadata(files, now_ns() - started);
    for (const auto& item : extra) metadata[item.key()] = item.value();
    return metadata;
}
Object FirecrackerBackend::prepare_round(const VmHandle&, int) {
    return {{"snapshot_policy", "per-fork"}};
}
CloneResult FirecrackerBackend::clone(const VmHandle& root, int id) {
    const auto prefix = work_ / ("c" + std::to_string(id));
    const auto [entry, inserted] = snapshots_.emplace(id, SnapshotFiles{prefix.string() + ".state", prefix.string() + ".mem"});
    require(inserted, "Snapshot ID reused before child cleanup");
    const auto& files = entry->second;
    auto metadata = prepare_snapshot(root, files);
    auto child = start_process(id);
    firecracker_request(child->control, "PUT", "/snapshot/load", {{"snapshot_path", files.state.string()},
        {"mem_backend", Object{{"backend_type", "File"}, {"backend_path", files.memory.string()}}},
        {"vsock_override", Object{{"uds_path", child->vsock.string()}}}, {"track_dirty_pages", false}, {"resume_vm", true}});
    return {child, std::move(metadata)};
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
    if (const auto entry = snapshots_.find(vm->id); entry != snapshots_.end()) {
        remove_snapshot_files(entry->second);
        snapshots_.erase(entry);
    }
    remove_socket_files(vm);
}
void FirecrackerBackend::remove_snapshot_files(const SnapshotFiles& files) const {
    fs::remove(files.state);
    fs::remove(files.memory);
}
void FirecrackerBackend::release_round(const VmHandle&) {
    require(snapshots_.empty(), "Snapshot backing remains after round cleanup");
}
json::array FirecrackerBackend::cleanup() {
    auto errors = ManagedBackend::cleanup();
    try {
        if (processes_.active().empty()) {
            for (const auto& item : snapshots_) remove_snapshot_files(item.second);
            snapshots_.clear();
        }
        if (!base_.empty()) { fs::remove(base_); base_.clear(); }
    }
    catch (const std::exception& error) { errors.push_back(json::value(error.what())); }
    return errors;
}
}

namespace vmbench {
std::unique_ptr<Backend> make_backend(const Options& options, const fs::path& work) {
    if (options.backend == "lightning") return std::make_unique<LightningBackend>(options, work);
    if (options.backend == "firecracker") return std::make_unique<FirecrackerBackend>(options, work);
    throw std::runtime_error("Choose --backend lightning or firecracker");
}
}
