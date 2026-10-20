#include "benchmark.hpp"
#include <charconv>
#include <cstdlib>
#include <iostream>
#include <ostream>
#include <sys/prctl.h>
#include <sys/utsname.h>
#include <unistd.h>



namespace vmbench {
namespace {
int parse_integer(const std::string& value) {
    int result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    require(error == std::errc{} && end == value.data() + value.size(), "Invalid integer: " + value);
    return result;
}
void assign_option(Options& options, const std::string& key, const std::string& value) {
    if (key == "--repo") options.repo = value;
    else if (key == "--backend") options.backend = value;
    else if (key == "--lightning") options.lightning = value;
    else if (key == "--firecracker") options.firecracker = value;
    else if (key == "--kernel") options.kernel = value;
    else if (key == "--initrd") options.initrd = value;
    else if (key == "--output") options.output = value;
    else if (key == "--rounds") options.rounds = parse_integer(value);
    else if (key == "--forks-per-round") options.per_round = parse_integer(value);
    else if (key == "--memory-mib") options.memory_mib = parse_integer(value);
    else if (key == "--vcpus") options.vcpus = parse_integer(value);
    else if (key == "--settle-ms") options.settle_ms = parse_integer(value);
    else if (key == "--huge-pages") options.huge_pages = value;
    else if (key == "--work-dir") options.work_dir = value;
    else if (key == "--cgroup-dir") options.cgroup = value;
    else throw std::runtime_error("Unknown option " + key);
}
void validate_options(const Options& options) {
    require(options.rounds > 0 && options.rounds <= 1000, "Invalid round count");
    require(options.per_round > 0 && options.per_round <= 1000, "Invalid batch size");
    require(options.vcpus > 0 && options.vcpus <= 32, "Invalid vCPU count");
    require(options.memory_mib >= 128, "Guest capacity must be at least 128 MiB");
    require(options.settle_ms >= 0 && options.settle_ms <= 10000, "Invalid settling delay");
    require(options.huge_pages == "None" || options.huge_pages == "Transparent", "Invalid hugepage policy");
}
void resolve_paths(Options& options) {
    options.repo = fs::absolute(options.repo);
    if (options.lightning.empty()) options.lightning = options.repo / "build/cargo_target/release/lightning";
    if (options.firecracker.empty()) options.firecracker = options.repo / "build/cargo_target/release/firecracker";
    for (auto* path : {&options.lightning, &options.firecracker, &options.kernel, &options.initrd, &options.output, &options.work_dir, &options.cgroup}) {
        if (!path->empty()) *path = fs::absolute(*path);
    }
}
}
Options parse_options(int argc, char** argv) {
    Options options;
    const char* home = std::getenv("HOME");
    options.repo = fs::path(home ? home : ".") / "Projects/lightning-main";
    for (int index = 1; index < argc; ++index) {
        const std::string key = argv[index];
        if (key == "--help") { options.command = Command::Help; return options; }
        if (key == "--self-test") { options.command = Command::SelfTest; return options; }
        if (key == "--preflight") { options.command = Command::Preflight; continue; }
        require(index + 1 < argc, "Missing value for " + key);
        assign_option(options, key, argv[++index]);
    }
    validate_options(options);
    resolve_paths(options);
    return options;
}
void print_help(std::ostream& output) {
    output << "vm-fork-bench --backend lightning|firecracker --kernel VMLINUX --initrd INITRD --output NEW_DIR\n"
              "  [--repo DIR] [--lightning BIN] [--firecracker BIN] [--rounds 50] [--forks-per-round 50]\n"
              "  [--memory-mib 256] [--vcpus 1] [--huge-pages None|Transparent] [--settle-ms 200]\n"
              "  [--work-dir /tmp] [--cgroup-dir DIR] [--preflight] | --self-test\n";
}
}

namespace vmbench {
Object inspect_host(const Options& options) {
    utsname host{};
    require(uname(&host) == 0, "Cannot inspect host");
    return {{"host_kernel", host.release}, {"architecture", host.machine},
        {"kvm_accessible", access("/dev/kvm", R_OK | W_OK) == 0},
        {"lightning_binary_exists", fs::is_regular_file(options.lightning)},
        {"firecracker_binary_exists", fs::is_regular_file(options.firecracker)},
        {"kernel_exists", fs::is_regular_file(options.kernel)}, {"initrd_exists", fs::is_regular_file(options.initrd)},
        {"rounds", options.rounds}, {"forks_per_round", options.per_round},
        {"total_forks", options.rounds * options.per_round}, {"maximum_live_vms", options.per_round + 1},
        {"memory_mib", options.memory_mib}, {"vcpus", options.vcpus}, {"huge_pages", options.huge_pages},
        {"cgroup", options.cgroup.string()}, {"thp_policy", optional_text("/sys/kernel/mm/transparent_hugepage/enabled")}};
}
void validate_runtime(const Options& options, const Object& inspection) {
    require(options.backend == "lightning" || options.backend == "firecracker", "Choose --backend lightning or firecracker");
    require(inspection.at("kvm_accessible").as_bool(), "No accessible /dev/kvm; use a KVM-enabled Linux host");
    require(text(inspection.at("architecture")) == "x86_64", "This checkout's live-fork path is validated on x86_64");
    require(fs::is_regular_file(options.kernel) && fs::is_regular_file(options.initrd), "Supply a compatible --kernel and --initrd");
    require(fs::is_regular_file(options.backend == "lightning" ? options.lightning : options.firecracker), "VMM binary is missing; build it first");
    require(!options.output.empty(), "Supply --output NEW_DIR");
    require(!fs::exists(options.output), "Output directory already exists");
}
}

namespace vmbench {
namespace {
int run_test_executable() {
    const auto executable = fs::read_symlink("/proc/self/exe").parent_path() / "vm-fork-bench-tests";
    require(fs::is_regular_file(executable), "Test executable missing; run make check");
    execl(executable.c_str(), executable.c_str(), static_cast<char*>(nullptr));
    throw std::runtime_error("Cannot execute tests");
}
Object initial_summary(const Options& options, const Object& inspection) {
    return {{"backend", options.backend}, {"preflight", inspection}, {"status", "running"}, {"language", "C++20"},
        {"controller_pid", getpid()}, {"source_repo", options.repo.string()}, {"kernel", options.kernel.string()},
        {"initrd", options.initrd.string()}, {"binary", (options.backend == "lightning" ? options.lightning : options.firecracker).string()},
        {"allocation_policy", "No added buffer; minimal booted guest and round counter only"}, {"mounts", optional_text("/proc/mounts")}};
}
int execute_benchmark(Backend& backend, const Options& options, Recorder& recorder, Object& summary) {
    try {
        Benchmark benchmark(backend, options, [&](const MeasurementEvent& event) { recorder.record(event); });
        summary["result"] = benchmark.run();
        summary["status"] = "ok";
        return 0;
    } catch (const std::exception& error) {
        summary["status"] = "failed";
        summary["error"] = error.what();
        return 1;
    }
}
bool finalize_run(Backend& backend, const Options& options, Recorder& recorder, Object& summary) {
    const auto errors = backend.cleanup();
    summary["cleanup_errors"] = errors;
    if (!errors.empty()) summary["status"] = "cleanup_failed";
    sleep_ms(options.settle_ms);
    recorder.record({errors.empty() ? "all_stopped" : "cleanup_incomplete", options.rounds, 0,
        options.rounds * options.per_round, {}, {}});
    summary["metrics"] = recorder.report();
    write_text(options.output / "summary.json", json::serialize(summary) + "\n");
    return errors.empty();
}
int run_measurement(const Options& options, const Object& inspection) {
    validate_runtime(options, inspection);
    fs::create_directories(options.output);
    require(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0, "Cannot enable child subreaper");
    install_signal_handlers();
    TemporaryDirectory temporary(options.work_dir);
    auto backend = make_backend(options, temporary.path());
    Recorder recorder(options);
    auto summary = initial_summary(options, inspection);
    recorder.record({"before_boot", 0, 0, 0, {}, {}});
    const auto status = execute_benchmark(*backend, options, recorder, summary);
    return finalize_run(*backend, options, recorder, summary) ? status : 1;
}
int dispatch(Options options) {
    if (options.command == Command::Help) { print_help(std::cout); return 0; }
    if (options.command == Command::SelfTest) return run_test_executable();
    if (options.cgroup.empty()) options.cgroup = find_cgroup();
    const auto inspection = inspect_host(options);
    if (options.command == Command::Preflight) { std::cout << json::serialize(inspection) << '\n'; return 0; }
    return run_measurement(options, inspection);
}
}
int run_cli(int argc, char** argv) {
    try { return dispatch(parse_options(argc, argv)); }
    catch (const std::exception& error) { std::cerr << "Error: " << error.what() << '\n'; return 1; }
}
}

#ifndef VMBENCH_TESTING
int main(int argc, char** argv) {
    return vmbench::run_cli(argc, argv);
}
#endif
