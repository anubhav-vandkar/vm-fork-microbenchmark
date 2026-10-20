#include "vmbench/application.hpp"
#include "vmbench/backend.hpp"
#include "vmbench/benchmark.hpp"
#include "vmbench/cgroup_reader.hpp"
#include "vmbench/error.hpp"
#include "vmbench/options.hpp"
#include "vmbench/preflight.hpp"
#include "vmbench/recorder.hpp"
#include "vmbench/signals.hpp"
#include "vmbench/temporary_directory.hpp"
#include "vmbench/timing.hpp"
#include <iostream>
#include <sys/prctl.h>
#include <unistd.h>

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
