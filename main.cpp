#include "benchmark.hpp"
#include <charconv>
#include <cstdlib>
#include <iostream>
#include <ostream>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <unistd.h>



namespace vmbench {
namespace {
template<class Integer = int>
Integer parse_integer(const std::string& value) {
    Integer result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    require(error == std::errc{} && end == value.data() + value.size(), "Invalid integer: " + value);
    return result;
}
void assign_option(Options& options, const std::string& key, const std::string& value) {
    if (key == "--repo" || key == "--lightning-source") options.repo = value;
    else if (key == "--firecracker-source") options.firecracker_repo = value;
    else if (key == "--backend") options.backend = value;
    else if (key == "--experiment") options.experiment = value;
    else if (key == "--depth") options.depth = parse_integer(value);
    else if (key == "--children") options.children = parse_integer(value);
    else if (key == "--duration-s") options.duration_s = parse_integer(value);
    else if (key == "--populate-mib") options.populate_mib = parse_integer(value);
    else if (key == "--cxl-pool") options.cxl_pool = value;
    else if (key == "--cxl-offset") options.cxl_offset = parse_integer<uint64_t>(value);
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
    else if (key == "--snapshot-type") options.snapshot_type = value;
    else if (key == "--work-dir") options.work_dir = value;
    else if (key == "--cgroup-dir") options.cgroup = value;
    else throw std::runtime_error("Unknown option " + key);
}
void validate_options(const Options& options) {
    require(options.experiment == "batch" || options.experiment == "depth" || options.experiment == "footprint",
        "Use --experiment batch, depth or footprint");
    require(options.memory_mib >= 128, "Guest capacity must be at least 128 MiB");
    if (options.experiment == "depth")
        require(options.depth >= 1 && options.depth <= 64, "Figure 4(b) depth must be between 1 and 64");
    if (options.experiment == "footprint") {
        require(options.children >= 1 && options.children <= 16, "Figure 4(a) needs 1 to 16 children");
        require(options.duration_s >= 1 && options.duration_s <= 60, "Figure 4(a) duration must be 1 to 60 seconds");
    }
    if (options.experiment != "batch") {
        require(options.populate_mib > 0 && options.populate_mib <= options.memory_mib - 64,
            "Population must be positive and leave at least 64 MiB for the guest kernel");
        require(options.huge_pages == "Transparent", "Figure 4 requires --huge-pages Transparent");
    }
    if (options.backend == "lightning-cxl") {
        require(options.experiment != "batch", "Lightning CXL is supported in the Figure 4 experiments");
        require(options.memory_mib % 2 == 0, "CXL guest capacity must be a multiple of 2 MiB");
        if (options.cxl_standin) {
            require(options.cxl_pool.empty() && !options.cxl_offset, "The private stand-in takes no pool path or offset");
        } else {
            require(!options.cxl_pool.empty() && options.cxl_offset.has_value(),
                "Supply --cxl-pool DEVICE and --cxl-offset BYTES for a reserved devdax range");
            constexpr uint64_t chunk = 2ULL << 20;
            const auto bytes = static_cast<uint64_t>(options.memory_mib) << 20;
            require(*options.cxl_offset % chunk == 0 && *options.cxl_offset <= INT64_MAX - bytes,
                "CXL offset must be 2 MiB aligned and the range must fit in signed 64-bit offsets");
        }
    } else {
        require(options.cxl_pool.empty() && !options.cxl_offset && !options.cxl_standin,
            "CXL options require --backend lightning-cxl");
    }
    require(options.rounds > 0 && options.rounds <= 1000, "Invalid round count");
    require(options.per_round > 0 && options.per_round <= 1000, "Invalid batch size");
    require(options.vcpus > 0 && options.vcpus <= 32, "Invalid vCPU count");
    require(options.settle_ms >= 0 && options.settle_ms <= 10000, "Invalid settling delay");
    require(options.huge_pages == "None" || options.huge_pages == "Transparent", "Invalid hugepage policy");
    require(options.snapshot_type == "full" || options.snapshot_type == "diff", "Use --snapshot-type full or diff");
}
void resolve_paths(Options& options) {
    options.repo = fs::absolute(options.repo);
    options.firecracker_repo = fs::absolute(options.firecracker_repo);
    if (options.lightning.empty()) options.lightning = options.repo / "build/cargo_target/release/lightning";
    if (options.firecracker.empty()) options.firecracker = options.firecracker_repo / "build/cargo_target/release/firecracker";
    for (auto* path : {&options.lightning, &options.firecracker, &options.kernel, &options.initrd, &options.output, &options.work_dir, &options.cgroup, &options.cxl_pool}) {
        if (!path->empty()) *path = fs::absolute(*path);
    }
}
int planned_forks(const Options& options) {
    if (options.experiment == "depth") return options.depth;
    if (options.experiment == "footprint") return options.children;
    return options.rounds * options.per_round;
}
}
Options parse_options(int argc, char** argv) {
    Options options;
    if (argc > 1 && (std::string(argv[1]) == "--report-only" || std::string(argv[1]) == "--depth-report" ||
                    std::string(argv[1]) == "--figure4-report")) {
        require(argc == 3, "Usage: vm-fork-bench --report-only or --figure4-report RUN_DIRECTORY");
        options.command = std::string(argv[1]) == "--report-only" ? Command::ReportOnly : Command::Figure4Report;
        options.output = fs::absolute(argv[2]);
        return options;
    }
    const char* home = std::getenv("HOME");
    options.repo = fs::path(home ? home : ".") / "lightning";
    options.firecracker_repo = fs::path(home ? home : ".") / "firecracker";
    bool page_policy_explicit = false;
    for (int index = 1; index < argc; ++index) {
        const std::string key = argv[index];
        if (key == "--help") { options.command = Command::Help; return options; }
        if (key == "--self-test") { options.command = Command::SelfTest; return options; }
        if (key == "--preflight") { options.command = Command::Preflight; continue; }
        if (key == "--cxl-standin") { options.cxl_standin = true; continue; }
        require(index + 1 < argc, "Missing value for " + key);
        if (key == "--huge-pages") page_policy_explicit = true;
        assign_option(options, key, argv[++index]);
    }
    if (options.experiment != "batch" && !page_policy_explicit) options.huge_pages = "Transparent";
    validate_options(options);
    resolve_paths(options);
    return options;
}
void print_help(std::ostream& output) {
    output << "vm-fork-bench --backend lightning|lightning-cxl|firecracker --kernel VMLINUX --initrd INITRD --output NEW_DIR\n"
              "  [--lightning-source DIR] [--firecracker-source DIR] [--lightning BIN] [--firecracker BIN]\n"
              "  [--repo DIR (alias for --lightning-source)] [--rounds 50] [--forks-per-round 50]\n"
              "  [--memory-mib 256] [--vcpus 1] [--huge-pages None|Transparent] [--settle-ms 200]\n"
              "  [--snapshot-type full|diff (default: diff, KVM dirty-page tracking)]\n"
              "  [--work-dir /tmp] [--cgroup-dir DIR] [--preflight] | --self-test\n"
              "  [--experiment batch|depth|footprint] [--depth 64] [--populate-mib 64]\n"
              "  [--children 16] [--duration-s 60] (footprint only)\n"
              "  [--cxl-pool DEVICE --cxl-offset BYTES | --cxl-standin]\n"
              "vm-fork-bench --report-only EXISTING_RUN_DIRECTORY\n"
              "vm-fork-bench --figure4-report EXISTING_SUITE_DIRECTORY\n";
}
}

namespace vmbench {
Object inspect_host(const Options& options) {
    utsname host{};
    require(uname(&host) == 0, "Cannot inspect host");
    Object inspection{{"host_kernel", host.release}, {"architecture", host.machine},
        {"kvm_accessible", access("/dev/kvm", R_OK | W_OK) == 0},
        {"lightning_binary_exists", fs::is_regular_file(options.lightning)},
        {"firecracker_binary_exists", fs::is_regular_file(options.firecracker)},
        {"kernel_exists", fs::is_regular_file(options.kernel)}, {"initrd_exists", fs::is_regular_file(options.initrd)},
        {"experiment", options.experiment}, {"depth", options.depth}, {"populate_mib", options.populate_mib},
        {"children", options.children}, {"duration_s", options.duration_s},
        {"rounds", options.rounds}, {"forks_per_round", options.per_round},
        {"firecracker_snapshot_policy", "per-fork"},
        {"firecracker_snapshot_type", options.snapshot_type},
        {"work_directory", options.work_dir.string()},
        {"lightning_source", options.repo.string()}, {"firecracker_source", options.firecracker_repo.string()},
        {"total_forks", planned_forks(options)},
        {"maximum_live_vms", (options.experiment == "batch" ? options.per_round : planned_forks(options)) + 1},
        {"memory_mib", options.memory_mib}, {"vcpus", options.vcpus}, {"huge_pages", options.huge_pages}, {"settle_ms", options.settle_ms},
        {"cgroup", options.cgroup.string()}, {"thp_policy", optional_text("/sys/kernel/mm/transparent_hugepage/enabled")}};
    if (options.backend == "lightning-cxl") {
        inspection["cxl"] = Object{{"backing", options.cxl_standin ? "file-standin" : "devdax"},
            {"pool_path", options.cxl_pool.string()}, {"offset_bytes", options.cxl_offset.value_or(0)}};
    }
    return inspection;
}
void validate_cxl_host(const Options& options) {
    const fs::path tracepoint = "/sys/kernel/tracing/events/kvm/kvm_page_fault";
    require(access((tracepoint / "id").c_str(), R_OK) == 0 && access((tracepoint / "format").c_str(), R_OK) == 0,
        "Lightning CXL requires access to the kvm:kvm_page_fault tracepoint in tracefs");
    if (options.cxl_standin) return;
    struct stat device{};
    require(stat(options.cxl_pool.c_str(), &device) == 0 && S_ISCHR(device.st_mode),
        "CXL pool must be a devdax character device");
    require(access(options.cxl_pool.c_str(), R_OK | W_OK) == 0, "CXL pool needs read/write access");
    const auto sysfs = fs::path("/sys/dev/char") /
        (std::to_string(major(device.st_rdev)) + ":" + std::to_string(minor(device.st_rdev)));
    require(fs::read_symlink(sysfs / "subsystem").filename() == "dax", "CXL pool must belong to the dax subsystem");
    require(std::stoull(read_text(sysfs / "align")) == (2ULL << 20), "CXL pool must have 2 MiB alignment");
    const auto end = *options.cxl_offset + (static_cast<uint64_t>(options.memory_mib) << 20);
    require(end <= std::stoull(read_text(sysfs / "size")), "CXL guest memory range exceeds the device capacity");
}
void validate_runtime(const Options& options, const Object& inspection) {
    require(options.backend == "lightning" || options.backend == "lightning-cxl" || options.backend == "firecracker",
        "Choose --backend lightning, lightning-cxl or firecracker");
    require(inspection.at("kvm_accessible").as_bool(), "No accessible /dev/kvm; use a KVM-enabled Linux host");
    require(text(inspection.at("architecture")) == "x86_64", "This checkout's live-fork path is validated on x86_64");
    require(fs::is_regular_file(options.kernel) && fs::is_regular_file(options.initrd), "Supply a compatible --kernel and --initrd");
    require(fs::is_regular_file(options.backend == "firecracker" ? options.firecracker : options.lightning), "VMM binary is missing; build it first");
    require(!options.output.empty(), "Supply --output NEW_DIR");
    require(!fs::exists(options.output), "Output directory already exists");
    if (options.backend == "lightning-cxl") validate_cxl_host(options);
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
    return {{"experiment", options.experiment}, {"backend", options.backend}, {"snapshot_policy", options.backend == "firecracker" ? "per-fork" : "live-fork"},
        {"snapshot_type", options.backend == "firecracker" ? options.snapshot_type : "none"},
        {"track_dirty_pages", options.backend == "firecracker" && options.snapshot_type == "diff"},
        {"creation_timing_includes_preparation", true}, {"preflight", inspection}, {"status", "running"}, {"language", "C++20"},
        {"controller_pid", getpid()}, {"source_repo", options.repo.string()}, {"kernel", options.kernel.string()},
        {"firecracker_source_repo", options.firecracker_repo.string()},
        {"initrd", options.initrd.string()}, {"binary", (options.backend == "firecracker" ? options.firecracker : options.lightning).string()},
        {"allocation_policy", options.experiment == "batch"
            ? "No added buffer; minimal booted guest and round counter only"
            : "One inherited populated buffer; first page modified in children for isolation checks"}, {"mounts", optional_text("/proc/mounts")}};
}
int execute_benchmark(Backend& backend, const Options& options, Recorder& recorder, Object& summary) {
    try {
        const auto observer = [&](const MeasurementEvent& event) { recorder.record(event); };
        if (options.experiment == "depth") {
            DepthBenchmark benchmark(backend, options, observer);
            summary["result"] = benchmark.run();
        } else if (options.experiment == "footprint") {
            FootprintBenchmark benchmark(backend, options, observer);
            summary["result"] = benchmark.run();
        } else {
            Benchmark benchmark(backend, options, observer);
            summary["result"] = benchmark.run();
        }
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
    recorder.record({errors.empty() ? "all_stopped" : "cleanup_incomplete",
        options.experiment == "batch" ? options.rounds : 0, 0, planned_forks(options), {}, {}});
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
    if (options.command == Command::Figure4Report) {
        generate_figure4_report(options.output);
        std::cout << "Figure 4 reporting passed. Results: " << options.output << '\n';
        return 0;
    }
    if (options.command == Command::ReportOnly) {
        const auto project = fs::read_symlink("/proc/self/exe").parent_path().parent_path();
        generate_saved_report(options.output, project / "templates/results-dashboard.html");
        std::cout << "Reporting passed; no VMs started. Results: " << options.output << '\n';
        return 0;
    }
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
