#pragma once

// Shared types and interfaces for this small benchmark. Implementations live in the named .cpp files.

#include <boost/json.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace vmbench {
namespace json = boost::json;
using Object = json::object;
int64_t integer(const json::value& value);
std::string text(const json::value& value);
}

namespace vmbench {
inline void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
}

namespace vmbench {
namespace fs = std::filesystem;
std::string read_text(const fs::path& path);
json::value optional_text(const fs::path& path);
void write_text(const fs::path& path, const std::string& contents);
}

namespace vmbench {
using Clock = std::chrono::steady_clock;
uint64_t now_ns();
void sleep_ms(int milliseconds);
}

namespace vmbench {
void install_signal_handlers();
void check_interrupted();
}

namespace vmbench {
class FileDescriptor {
public:
    explicit FileDescriptor(int descriptor = -1) : descriptor_(descriptor) {}
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) reset(std::exchange(other.descriptor_, -1));
        return *this;
    }
    ~FileDescriptor() { reset(); }
    int get() const { return descriptor_; }
    void reset(int descriptor = -1) {
        if (descriptor_ >= 0) ::close(descriptor_);
        descriptor_ = descriptor;
    }
private:
    int descriptor_;
};
}

namespace vmbench {
enum class Command { Run, Help, Preflight, SelfTest };
struct Options {
    fs::path repo, lightning, firecracker, kernel, initrd, output, work_dir = "/tmp", cgroup;
    std::string backend, huge_pages = "None";
    int rounds = 50, per_round = 50, memory_mib = 256, vcpus = 1, settle_ms = 200;
    Command command = Command::Run;
};
Options parse_options(int argc, char** argv);
void print_help(std::ostream& output);
}

namespace vmbench {
struct Vm {
    int id = 0;
    pid_t pid = -1;
    FileDescriptor pidfd;
    bool alive = true;
    std::filesystem::path control, vsock;
};
using VmHandle = std::shared_ptr<Vm>;
using VmList = std::vector<VmHandle>;
}

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

namespace vmbench {
class TemporaryDirectory {
public:
    explicit TemporaryDirectory(const std::filesystem::path& parent);
    ~TemporaryDirectory();
    const std::filesystem::path& path() const { return path_; }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
private:
    std::filesystem::path path_;
};
}

namespace vmbench {
struct CloneResult { VmHandle child; Object metadata; };
class Backend {
public:
    virtual ~Backend() = default;
    virtual VmHandle boot() = 0;
    virtual void marker(const VmHandle& root, int round) = 0;
    virtual Object ready(const VmHandle& vm) = 0;
    virtual Object prepare_round(const VmHandle& root, int round) = 0;
    virtual CloneResult clone(const VmHandle& root, int id) = 0;
    virtual void destroy(const VmHandle& vm) = 0;
    virtual void release_round(const VmHandle& root) = 0;
    virtual json::array cleanup() = 0;
};
std::unique_ptr<Backend> make_backend(const Options& options, const fs::path& work);
}

namespace vmbench {
FileDescriptor connect_unix(const fs::path& path);
void send_all(int descriptor, const std::string& data);
std::string read_line(int descriptor);
std::string read_http_headers(int descriptor);
}

namespace vmbench {
Object lightning_request(const fs::path& socket, const Object& request);
}

namespace vmbench {
std::string make_http_request(const std::string& method, const std::string& endpoint, const Object& body);
int http_status(const std::string& response);
void firecracker_request(const fs::path& socket, const std::string& method, const std::string& endpoint, const Object& body);
}

namespace vmbench {
Object guest_request(const fs::path& socket, const std::string& command = "GET");
}

namespace vmbench {
Object parse_numeric_lines(const std::string& input, bool colon_delimited);
Object read_process_metrics(const Vm& vm);
Object sum_process_memory(const json::array& processes);
}

namespace vmbench {
fs::path find_cgroup();
Object select_memory_categories(const Object& statistics);
Object read_cgroup_metrics(const fs::path& directory);
}

namespace vmbench {
Object read_kvm_metrics(const VmList& vms, const fs::path& directory = "/sys/kernel/debug/kvm");
}

namespace vmbench {
class SampleCollector {
public:
    explicit SampleCollector(fs::path cgroup) : cgroup_(std::move(cgroup)) {}
    Object collect(const VmList& vms, bool include_kvm_counters) const;
private:
    fs::path cgroup_;
};
}

namespace vmbench {
struct MeasurementEvent {
    std::string phase;
    int round = 0, position = 0, fork_number = 0;
    VmList vms;
    Object extra;
};
using Observer = std::function<void(const MeasurementEvent&)>;
class Benchmark {
public:
    Benchmark(Backend& backend, const Options& options, Observer observer);
    Object run();
private:
    void run_round(int round);
    void create_child(int round, int position);
    void destroy_children();
    void verify_state(const Object& state, int round) const;
    void emit(const std::string& phase, int round, int position, Object extra = {});
    Backend& backend_;
    const Options& options_;
    Observer observer_;
    VmList active_;
    int completed_ = 0;
};
}

namespace vmbench {
class CsvWriter {
public:
    explicit CsvWriter(const fs::path& path);
    void write(const Object& sample);
private:
    std::ofstream output_;
};
}

namespace vmbench {
class Statistics {
public:
    void observe(const Object& sample);
    Object report() const;
private:
    std::vector<uint64_t> readiness_times_;
    std::optional<int64_t> root_pss_, peak_pss_, final_pss_;
};
}

namespace vmbench {
class Recorder {
public:
    explicit Recorder(const Options& options);
    void record(const MeasurementEvent& event);
    Object report() const { return statistics_.report(); }
private:
    Object collect_event(const MeasurementEvent& event) const;
    void write_json(const Object& sample);
    void print_progress(const MeasurementEvent& event, const Object& sample) const;
    const Options& options_;
    SampleCollector collector_;
    std::ofstream raw_;
    CsvWriter csv_;
    Statistics statistics_;
};
}

namespace vmbench {
Object inspect_host(const Options& options);
void validate_runtime(const Options& options, const Object& inspection);
}

namespace vmbench {
int run_cli(int argc, char** argv);
}
