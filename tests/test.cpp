#include "benchmark.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <iostream>
#include <sys/socket.h>
#include <sys/un.h>
#include <cstring>
#include <unistd.h>
#include <utility>
#include <vector>
#include <sstream>
#include <thread>
#include <sys/stat.h>



namespace vmbench::tests {
struct Skipped { std::string reason; };
namespace { void require_socket_access(); }
void test_batch_lifecycle();
void test_inheritance_failure();
void test_proc_parser();
void test_live_process_collection();
void test_cgroup_filter();
void test_kvm_pid_matching();
void test_csv_output();
void test_statistics();
void test_cli_options();
void test_http_protocol();
void test_socket_framing();
void test_process_cleanup();
void test_json_round_trip();
void test_saved_report();
void test_incomplete_report();
void test_combined_creation_timing();
}

namespace vmbench::tests {
class FakeBackend final : public Backend {
public:
    explicit FakeBackend(int batch_size) : batch_size_(batch_size) {}
    VmHandle boot() override;
    void marker(const VmHandle& root, int round) override;
    Object ready(const VmHandle& vm) override;
    Object prepare_round(const VmHandle& root, int round) override;
    CloneResult clone(const VmHandle& root, int id) override;
    void destroy(const VmHandle& vm) override;
    void release_round(const VmHandle& root) override;
    json::array cleanup() override { return {}; }
    VmList nodes;
    bool corrupt_child = false;
private:
    int batch_size_, marker_ = 0, created_ = 0;
    bool prepared_ = false;
};
}

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

namespace vmbench::tests {
void test_batch_lifecycle() {
    Options options;
    options.settle_ms = 0;
    FakeBackend backend(50);
    int batch_samples = 0, clean_samples = 0, peak = 0;
    Benchmark benchmark(backend, options, [&](const MeasurementEvent& event) {
        require(std::all_of(event.vms.begin(), event.vms.end(), [](const auto& vm) { return vm->alive; }), "Sample has dead VM");
        peak = std::max(peak, static_cast<int>(event.vms.size()));
        if (event.phase == "batch_ready") { require(event.vms.size() == 51, "Wrong batch size"); ++batch_samples; }
        if (event.phase == "batch_destroyed") { require(event.vms.size() == 1, "Cleanup did not return to root"); ++clean_samples; }
    });
    const auto result = benchmark.run();
    require(integer(result.at("forks_completed")) == 2500 && backend.nodes.size() == 2501, "Wrong total fork count");
    require(peak == 51 && batch_samples == 50 && clean_samples == 50, "Wrong batch measurement boundaries");
}
void test_inheritance_failure() {
    Options options;
    options.rounds = 1;
    options.per_round = 2;
    options.settle_ms = 0;
    FakeBackend backend(2);
    backend.corrupt_child = true;
    Benchmark benchmark(backend, options, [](const MeasurementEvent&) {});
    try { benchmark.run(); }
    catch (const std::runtime_error& error) {
        require(std::string(error.what()).find("Wrong inherited state") != std::string::npos, "Unexpected validation failure");
        return;
    }
    throw std::runtime_error("Corrupted child state was accepted");
}
}

namespace vmbench::tests {
void test_proc_parser() {
    const auto parsed = parse_numeric_lines("Pss: 123 kB\nName: process\n", true);
    require(integer(parsed.at("Pss")) == 123 && parsed.size() == 1, "Proc parser failed");
}
void test_live_process_collection() {
    auto vm = std::make_shared<Vm>();
    vm->pid = getpid();
    const auto sample = SampleCollector({}).collect({vm}, false);
    require(integer(sample.at("memory_sum_kib").as_object().at("Pss")) > 0, "Live process PSS missing");
    require(sample.at("processes").as_array().front().as_object().contains("kvm_fds"), "KVM descriptor fields missing");
}
void test_cgroup_filter() {
    const auto categories = select_memory_categories({{"anon", 1024}, {"file", 2048}, {"sec_pagetables", 4096}, {"slab", 999}});
    require(categories.size() == 3 && !categories.contains("slab"), "Broad kernel profiling was introduced");
}
void test_kvm_pid_matching() {
    TemporaryDirectory temporary(fs::temp_directory_path());
    for (const auto* name : {"123-7", "1234-7"}) {
        const auto directory = temporary.path() / name;
        fs::create_directory(directory);
        write_text(directory / "pages_4k", "42\n");
    }
    auto vm = std::make_shared<Vm>();
    vm->pid = 123;
    const auto result = read_kvm_metrics({vm}, temporary.path());
    const auto& matches = result.at("123").as_object();
    require(matches.size() == 1 && matches.contains("123-7"), "KVM stats mixed PID prefixes");
}
}

namespace vmbench::tests {
namespace {
Object make_sample(const std::string& phase, int pss, int readiness = 0) {
    return {{"phase", phase}, {"round", 1}, {"position", 1}, {"fork_number", 1}, {"live_vms", 2},
        {"memory_sum_kib", Object{{"Pss", pss}, {"Rss", pss + 10}, {"Private_Dirty", 5}}},
        {"cgroup", nullptr}, {"extra", Object{{"guest_ready_ns", readiness}}}};
}
}
void test_csv_output() {
    TemporaryDirectory temporary(fs::temp_directory_path());
    const auto path = temporary.path() / "samples.csv";
    CsvWriter writer(path);
    writer.write(make_sample("child_ready", 123));
    const auto contents = read_text(path);
    require(contents.starts_with("phase,round,position,fork_number,live_vms,pss_kib,"), "CSV header changed");
    require(contents.find("child_ready,1,1,1,2,123,133,5,,\n") != std::string::npos, "CSV row fields changed");
}
void test_statistics() {
    Statistics statistics;
    require(statistics.report().at("root_pss_kib").is_null(), "Missing baseline is not null");
    statistics.observe(make_sample("root_ready", 100));
    statistics.observe(make_sample("batch_ready", 300));
    statistics.observe(make_sample("batch_ready", 250));
    statistics.observe(make_sample("batch_destroyed", 110));
    for (const auto latency : {30, 10, 20}) statistics.observe(make_sample("child_ready", 200, latency));
    const auto result = statistics.report();
    require(integer(result.at("maximum_batch_pss_kib")) == 300, "Peak batch PSS incorrect");
    require(integer(result.at("final_parent_pss_kib")) == 110, "Final parent PSS incorrect");
    require(integer(result.at("guest_ready_median_ns")) == 20, "Readiness median incorrect");
    require(integer(result.at("guest_ready_p95_ns")) == 30, "Readiness p95 incorrect");
}
}

namespace vmbench::tests {
namespace {
Options parse(std::vector<std::string> arguments) {
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    return parse_options(static_cast<int>(argv.size()), argv.data());
}
}
void test_cli_options() {
    const auto options = parse({"benchmark", "--preflight", "--rounds", "4", "--forks-per-round", "3"});
    require(options.command == Command::Preflight && options.rounds == 4 && options.per_round == 3, "CLI options changed");
    const auto report = parse({"benchmark", "--report-only", "."});
    require(report.command == Command::ReportOnly && report.output == fs::absolute("."), "Report-only CLI dispatch failed");
    const auto sources = parse({"benchmark", "--lightning-source", "/server/lightning", "--firecracker-source", "/server/firecracker"});
    require(sources.repo == "/server/lightning" && sources.firecracker_repo == "/server/firecracker" &&
        sources.lightning == "/server/lightning/build/cargo_target/release/lightning" &&
        sources.firecracker == "/server/firecracker/build/cargo_target/release/firecracker", "Source directories are not independent");
    const auto binaries = parse({"benchmark", "--repo", "/old/lightning", "--firecracker-source", "/server/fc", "--firecracker", "/custom/fc"});
    require(binaries.repo == "/old/lightning" && binaries.firecracker == "/custom/fc", "Legacy source alias or binary override changed");
    require(options.snapshot_type == "diff" && parse({"benchmark", "--snapshot-type", "full"}).snapshot_type == "full", "Snapshot mode defaults/override changed");
    bool rejected = false;
    try { parse({"benchmark", "--snapshot-type", "invalid"}); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "Invalid snapshot mode accepted");
    try { parse({"benchmark", "--rounds", "4invalid"}); }
    catch (const std::runtime_error&) { return; }
    throw std::runtime_error("Malformed integer option accepted");
}
}

namespace vmbench::tests {
void test_http_protocol() {
    const auto request = make_http_request("PUT", "/snapshot/load", {{"resume_vm", true}});
    require(request.starts_with("PUT /snapshot/load HTTP/1.1\r\n"), "HTTP request line changed");
    const auto payload = json::serialize(Object{{"resume_vm", true}});
    require(request.ends_with("\r\n\r\n" + payload), "HTTP JSON body changed");
    require(request.find("Content-Length: " + std::to_string(payload.size())) != std::string::npos, "HTTP content length incorrect");
    require(http_status("HTTP/1.1 204 No Content\r\n\r\n") == 204, "HTTP status parser incorrect");
}
void test_socket_framing() {
    int pair[2];
    require(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0, "socketpair failed");
    FileDescriptor sender(pair[0]), receiver(pair[1]);
    const Object original{{"ok", true}, {"path", "/tmp/a\"b"}};
    const auto payload = json::serialize(original) + "\n";
    const auto sent = send(sender.get(), payload.data(), payload.size(), MSG_NOSIGNAL);
    if (sent < 0 && errno == EPERM) throw Skipped{"Unix-socket send denied by execution sandbox (EPERM)"};
    require(sent == static_cast<ssize_t>(payload.size()), "Socket framing write failed");
    require(json::parse(read_line(receiver.get())).as_object() == original, "Socket JSON framing failed");
}
void test_json_round_trip() {
    const auto original = json::parse("{\"ok\":true,\"path\":\"/tmp/a\\\"b\"}");
    require(json::parse(json::serialize(original)) == original, "JSON round-trip failed");
}
}

namespace vmbench::tests {
void test_process_cleanup() {
    TemporaryDirectory temporary(fs::temp_directory_path());
    ProcessManager processes;
    const auto pid = processes.launch({"/bin/sleep", "30"}, temporary.path() / "process.log");
    auto vm = processes.track(1, pid, temporary.path() / "api", temporary.path() / "vsock");
    processes.stop(vm, SIGTERM);
    require(!vm->alive && vm->pidfd.get() < 0, "VM process descriptor retained after stop");
    require(processes.active().empty() && processes.launched_count() == 0, "Exited process records retained");
    processes.stop(vm, SIGTERM);
}
}

namespace vmbench::tests {
namespace {
void saved_fixture(const fs::path& directory, bool complete) {
    std::ofstream output(directory / "samples.jsonl");
    Statistics statistics;
    const std::vector<std::string> phases = {"before_boot", "root_ready", "round_before", "child_ready", "batch_ready", "batch_destroyed", "all_stopped"};
    for (size_t index = 0; index < phases.size() - (complete ? 0 : 1); ++index) {
        const bool round = index >= 2;
        const int live = index == 0 || index == 6 ? 0 : index == 3 || index == 4 ? 2 : 1;
        auto sample = make_sample(phases[index], live * 1024, 5000000);
        sample["round"] = round ? 1 : 0;
        sample["position"] = index == 3 || index == 4 ? 1 : 0;
        sample["fork_number"] = index >= 3 ? 1 : 0;
        sample["live_vms"] = live;
        sample["monotonic_ns"] = index * 1000000;
        json::array processes;
        for (int vm = 0; vm < live; ++vm) processes.push_back(Object{{"kvm_vm_fd_count", 1}, {"kvm_vcpu_fd_alias_count", 2}, {"threads", 3}, {"fd_count", 9}});
        sample["processes"] = std::move(processes);
        statistics.observe(sample);
        output << json::serialize(sample) << '\n';
    }
    write_text(directory / "summary.json", json::serialize(Object{{"backend", "lightning"}, {"status", "ok"}, {"cleanup_errors", json::array{}},
        {"preflight", Object{{"rounds", 1}, {"forks_per_round", 1}, {"memory_mib", 256}, {"vcpus", 1},
            {"huge_pages", "None"}, {"host_kernel", "fixture"}, {"architecture", "x86_64"}}},
        {"result", Object{{"rounds_completed", 1}, {"forks_completed", 1}, {"maximum_live_vms", 2}}}, {"metrics", statistics.report()}}));
}
}
void test_saved_report() {
    TemporaryDirectory temporary(fs::temp_directory_path());
    saved_fixture(temporary.path(), true);
    const auto result = analyze_saved_backend(temporary.path());
    require(integer(result.at("forks_completed")) == 1 && integer(result.at("final_live_vms")) == 0, "Saved lifecycle counts changed");
    require(result.at("batch_pss_median").as_double() == 2.0 && result.at("added_pss_per_child_median").as_double() == 1.0, "Saved PSS aggregation failed");
    require(result.at("final_sec_pagetables").is_null() && !result.contains("kvm_sec_pagetables_batch_max"), "Unavailable KVM measurements became zero");
    require(result.at("amortized_creation_mean_ms").as_double() == 5.0, "Live fork combined creation time changed");

    // Exercise complete report generation, including policy labels in the dashboard.
    for (const auto* backend : {"lightning", "firecracker"}) {
        fs::create_directory(temporary.path() / backend);
        saved_fixture(temporary.path() / backend, true);
    }
    const auto fc = temporary.path() / "firecracker";
    auto summary = json::parse(read_text(fc / "summary.json")).as_object();
    summary["backend"] = "firecracker";
    summary["snapshot_policy"] = "per-fork";
    summary["creation_timing_includes_preparation"] = true;
    Statistics statistics;
    std::istringstream input(read_text(fc / "samples.jsonl"));
    std::ostringstream raw;
    std::string line;
    while (std::getline(input, line)) {
        auto sample = json::parse(line).as_object();
        auto& extra = sample["extra"].as_object();
        if (text(sample.at("phase")) == "child_ready") {
            extra["guest_ready_ns"] = 25000000;
            extra["backend_metadata"] = Object{{"snapshot_policy", "per-fork"}, {"prepare_ns", 20000000},
                {"preparation_included_in_guest_ready", true}, {"snapshot_path", "s1"}, {"snapshot_memory_path", "m1"}};
        }
        if (text(sample.at("phase")) == "batch_ready") extra["preparation"] = Object{{"snapshot_policy", "per-fork"}};
        statistics.observe(sample);
        raw << json::serialize(sample) << '\n';
    }
    summary["metrics"] = statistics.report();
    write_text(fc / "summary.json", json::serialize(summary));
    write_text(fc / "samples.jsonl", raw.str());
    const auto project = fs::read_symlink("/proc/self/exe").parent_path().parent_path();
    generate_saved_report(temporary.path(), project / "templates/results-dashboard.html");
    const auto dashboard = read_text(temporary.path() / "dashboard.html");
    require(dashboard.find("{{") == std::string::npos && dashboard.find("fresh full snapshot before every child") != std::string::npos,
        "Dashboard tokens or snapshot policy label are incorrect");
    const auto comparison = json::parse(read_text(temporary.path() / "comparison.json")).as_object();
    require(integer(comparison.at("vm_runs_started")) == 0 &&
        comparison.at("backends").as_object().at("firecracker").as_object().at("amortized_creation_mean_ms").as_double() == 25.0,
        "Report generation changed fresh snapshot timing");

    summary["snapshot_type"] = "diff";
    summary["track_dirty_pages"] = true;
    std::vector<Object> diff_samples;
    std::istringstream diff_input(raw.str());
    while (std::getline(diff_input, line)) diff_samples.push_back(json::parse(line).as_object());
    auto& metadata = diff_samples[3]["extra"].as_object()["backend_metadata"].as_object();
    metadata["snapshot_type"] = "diff";
    metadata["track_dirty_pages"] = true;
    metadata["capture_ns"] = 3000000;
    metadata["backing_copy_ns"] = 4000000;
    metadata["initial_base"] = true;
    metadata["base_memory_path"] = "base";
    metadata["snapshot_memory_bytes"] = 4194304;
    metadata["base_memory_bytes"] = 4194304;
    metadata["base_allocated_bytes"] = 8192;
    metadata["merge_in_capture"] = true;
    metadata["child_track_dirty_pages"] = false;
    metadata["snapshot_copy_method"] = "sparse-copy";
    auto save_diff = [&] {
        std::ostringstream stream;
        for (const auto& sample : diff_samples) stream << json::serialize(sample) << '\n';
        write_text(fc / "samples.jsonl", stream.str());
        write_text(fc / "summary.json", json::serialize(summary));
    };
    save_diff();
    generate_saved_report(temporary.path(), project / "templates/results-dashboard.html");
    const auto diff_result = analyze_saved_backend(fc);
    require(diff_result.at("amortized_creation_mean_ms").as_double() == 25.0 && integer(diff_result.at("initial_base_snapshots")) == 1 &&
        integer(diff_result.at("sparse_copies")) == 1 && diff_result.at("snapshot_base_allocated_mib_median").as_double() == 8192.0 / 1048576 &&
        diff_result.at("snapshot_capture_ms_median").as_double() == 3.0 && diff_result.at("snapshot_backing_copy_ms_median").as_double() == 4.0 &&
        read_text(temporary.path() / "dashboard.html").find("fresh KVM dirty-page diff") != std::string::npos,
        "Diff report timing, allocations, base/copy counts or mode label are incorrect");
    auto reject_diff = [&] {
        save_diff();
        try { analyze_saved_backend(fc); } catch (const std::exception&) { return; }
        throw std::runtime_error("Invalid saved diff metadata accepted");
    };
    metadata["track_dirty_pages"] = false;
    reject_diff();
    metadata["track_dirty_pages"] = true;
    metadata["initial_base"] = false;
    reject_diff();
    metadata["initial_base"] = true;
    metadata["base_memory_path"] = "m1";
    reject_diff();
    metadata["base_memory_path"] = "base";
    metadata["capture_ns"] = 20000000;
    reject_diff();
}
void test_incomplete_report() {
    TemporaryDirectory temporary(fs::temp_directory_path());
    saved_fixture(temporary.path(), false);
    try { analyze_saved_backend(temporary.path()); }
    catch (const std::runtime_error& error) {
        require(std::string(error.what()).find("Incomplete saved lifecycle") != std::string::npos, "Wrong incomplete-run rejection");
        return;
    }
    throw std::runtime_error("Incomplete run accepted for reporting");
}
void test_combined_creation_timing() {
    TemporaryDirectory temporary(fs::temp_directory_path());
    saved_fixture(temporary.path(), true);
    std::vector<Object> samples;
    std::istringstream input(read_text(temporary.path() / "samples.jsonl"));
    std::string line;
    while (std::getline(input, line)) samples.push_back(json::parse(line).as_object());
    auto second_child = samples[3];
    second_child["position"] = 2;
    second_child["fork_number"] = 2;
    second_child["live_vms"] = 3;
    second_child["extra"].as_object()["guest_ready_ns"] = 15000000;
    second_child["processes"].as_array().push_back(second_child.at("processes").as_array().front());
    samples.insert(samples.begin() + 4, std::move(second_child));
    auto& batch = samples[5];
    batch["position"] = 2;
    batch["fork_number"] = 2;
    batch["live_vms"] = 3;
    batch["processes"].as_array().push_back(batch.at("processes").as_array().front());
    batch["extra"].as_object()["preparation"] = Object{{"prepare_ns", 20000000}};
    samples[6]["fork_number"] = 2;
    samples[7]["fork_number"] = 2;
    Statistics statistics;
    std::ostringstream raw;
    for (const auto& sample : samples) { statistics.observe(sample); raw << json::serialize(sample) << '\n'; }
    write_text(temporary.path() / "samples.jsonl", raw.str());
    auto summary = json::parse(read_text(temporary.path() / "summary.json")).as_object();
    summary["backend"] = "firecracker";
    summary["preflight"].as_object()["forks_per_round"] = 2;
    summary["result"].as_object()["forks_completed"] = 2;
    summary["result"].as_object()["maximum_live_vms"] = 3;
    summary["metrics"] = statistics.report();
    write_text(temporary.path() / "summary.json", json::serialize(summary));
    const auto result = analyze_saved_backend(temporary.path());
    require(result.at("combined_creation_total_ms").as_double() == 40.0 &&
        result.at("amortized_creation_mean_ms").as_double() == 20.0,
        "Combined timing must add preparation once and use raw child durations, not medians");

    // The same source fixture now models two fresh snapshots inside child timers.
    summary["snapshot_policy"] = "per-fork";
    summary["creation_timing_includes_preparation"] = true;
    samples[5]["extra"].as_object()["preparation"] = Object{{"snapshot_policy", "per-fork"}};
    for (size_t index : {3, 4}) {
        auto& extra = samples[index]["extra"].as_object();
        extra["guest_ready_ns"] = integer(extra.at("guest_ready_ns")) + 20000000;
        extra["backend_metadata"] = Object{{"snapshot_policy", "per-fork"}, {"prepare_ns", 20000000},
            {"preparation_included_in_guest_ready", true}, {"snapshot_path", "state" + std::to_string(index)},
            {"snapshot_memory_path", "memory" + std::to_string(index)}};
    }
    auto save = [&] {
        Statistics updated;
        std::ostringstream stream;
        for (const auto& sample : samples) { updated.observe(sample); stream << json::serialize(sample) << '\n'; }
        summary["metrics"] = updated.report();
        write_text(temporary.path() / "samples.jsonl", stream.str());
        write_text(temporary.path() / "summary.json", json::serialize(summary));
    };
    save();
    const auto fresh = analyze_saved_backend(temporary.path());
    require(fresh.at("combined_creation_total_ms").as_double() == 60.0 &&
        fresh.at("amortized_creation_mean_ms").as_double() == 30.0 && integer(fresh.at("snapshot_preparations")) == 2,
        "Fresh preparation must be charged once per child without double counting");
    auto reject = [&] {
        save();
        try { analyze_saved_backend(temporary.path()); }
        catch (const std::exception&) { return; }
        throw std::runtime_error("Invalid per-fork snapshot report accepted");
    };
    auto& second_metadata = samples[4]["extra"].as_object()["backend_metadata"].as_object();
    second_metadata["snapshot_path"] = "state3";
    reject();
    second_metadata["snapshot_path"] = "state4";
    second_metadata.erase("prepare_ns");
    reject();
    second_metadata["prepare_ns"] = 20000000;
    samples[5]["extra"].as_object()["preparation"].as_object()["prepare_ns"] = 20000000;
    reject();
}
}

namespace vmbench::tests {
namespace {
void write_page(const fs::path& path, std::streamoff offset, char value) {
    std::fstream output(path, std::ios::binary | std::ios::in | std::ios::out);
    require(bool(output), "Cannot open fixture memory");
    output.seekp(offset);
    output << std::string(4096, value);
    require(bool(output), "Cannot write fixture memory");
}
std::string read_page(const fs::path& path, std::streamoff offset) {
    std::ifstream input(path, std::ios::binary);
    input.seekg(offset);
    std::string page(4096, '\0');
    input.read(page.data(), page.size());
    require(bool(input), "Cannot read fixture memory");
    return page;
}
void make_diff_fixture(const fs::path& path, int capture) {
    if (capture == 1) {
        write_text(path, "");
        fs::resize_file(path, 4 * 1024 * 1024);
        write_page(path, 0, 'A'); write_page(path, 8192, 'P');
    }
    else if (capture == 2) { write_page(path, 0, '\0'); write_page(path, 65536, 'B'); }
    else write_page(path, 131072, 'C');
}

struct FakeGuestState {
    std::atomic<int> counter{0}, allocated_mib{0}, first{0};
};
void start_fake_guest(const fs::path& path, const std::shared_ptr<FakeGuestState>& state) {
    FileDescriptor server(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    require(path.string().size() < sizeof(address.sun_path), "Fake guest path too long");
    std::strcpy(address.sun_path, path.c_str());
    require(bind(server.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(server.get(), 4) == 0,
        "Cannot bind fake guest socket");
    std::thread([server = std::move(server), state] {
        try {
            for (;;) {
                FileDescriptor client(accept4(server.get(), nullptr, nullptr, SOCK_CLOEXEC));
                require(client.get() >= 0 && read_line(client.get()) == "CONNECT 5000", "Fake guest connection failed");
                send_all(client.get(), "OK 5000\n");
                const auto command = read_line(client.get());
                if (command.starts_with("SET ")) state->counter.store(std::stoi(command.substr(4)));
                if (command.starts_with("ALLOC ")) { state->allocated_mib.store(std::stoi(command.substr(6))); state->first.store(1); }
                if (command.starts_with("WRITE ")) state->first.store(std::stoi(command.substr(6)));
                const auto mib = state->allocated_mib.load();
                const auto checksum = mib ? populated_checksum(mib) - 1 + state->first.load() : 0;
                send_all(client.get(), json::serialize(Object{{"counter", state->counter.load()}, {"allocated", mib * 1048576},
                    {"first", state->first.load()}, {"checksum", command == "SCAN" ? checksum : 0}}) + "\n");
            }
        } catch (...) { _exit(2); }
    }).detach();
}
}

void test_sparse_diff_memory() {
    TemporaryDirectory temporary(fs::temp_directory_path());
    const auto base = temporary.path() / "base";
    make_diff_fixture(base, 1);
    for (const bool allow_reflink : {false, true}) {
        make_diff_fixture(base, 1);
        const auto copy = temporary.path() / (allow_reflink ? "auto" : "sparse");
        const auto method = copy_snapshot_memory(base, copy, allow_reflink);
        require(method == "sparse-copy" || (allow_reflink && method == "reflink"), "Incorrect backing-copy method");
        make_diff_fixture(base, 2);
        require(read_page(base, 0) == std::string(4096, '\0') && read_page(base, 8192) == std::string(4096, 'P') &&
            read_page(base, 65536) == std::string(4096, 'B'), "Diff zero/unchanged/new-page reconstruction failed");
        require(read_page(copy, 0) == std::string(4096, 'A') && read_page(copy, 8192) == std::string(4096, 'P') &&
            read_page(copy, 65536) == std::string(4096, '\0'), "Updating cumulative base corrupted earlier child memory");
        struct stat metadata{};
        require(stat(copy.c_str(), &metadata) == 0 && uint64_t(metadata.st_blocks) * 512 < fs::file_size(copy) / 4,
            "Child memory copy inflated sparse holes");
    }
    bool rejected = false;
    try { copy_snapshot_memory(base, temporary.path() / "sparse"); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "Existing child backing was overwritten");
}

// Local API simulator: exercises the real backend's request order and file ownership.
int fake_firecracker(const fs::path& socket_path) {
    FileDescriptor server(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    require(server.get() >= 0, "Fake API socket failed");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    require(socket_path.string().size() < sizeof(address.sun_path), "Fake API socket path too long");
    std::strcpy(address.sun_path, socket_path.c_str());
    require(bind(server.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(server.get(), 4) == 0,
        "Fake API bind/listen failed");
    bool paused = false, tracking = false;
    int captures = 0;
    auto state = std::make_shared<FakeGuestState>();
    for (;;) {
        FileDescriptor client(accept4(server.get(), nullptr, nullptr, SOCK_CLOEXEC));
        require(client.get() >= 0, "Fake API accept failed");
        const auto request = read_line(client.get());
        size_t length = 0;
        for (;;) {
            const auto header = read_line(client.get());
            if (header == "\r") break;
            if (header.starts_with("Content-Length: ")) length = std::stoul(header.substr(16));
        }
        std::string payload(length, '\0');
        for (size_t offset = 0; offset < length;) {
            const auto received = recv(client.get(), payload.data() + offset, length - offset, 0);
            require(received > 0, "Fake API body incomplete");
            offset += static_cast<size_t>(received);
        }
        const auto body = json::parse(payload).as_object();
        bool fail = false;
        if (request.starts_with("PATCH /vm ")) paused = text(body.at("state")) == "Paused";
        else if (request.starts_with("PUT /machine-config ")) tracking = body.at("track_dirty_pages").as_bool();
        else if (request.starts_with("PUT /vsock ")) start_fake_guest(text(body.at("uds_path")), state);
        else if (request.starts_with("PUT /boot-source ") || request.starts_with("PUT /actions ")) { /* Boot fixture. */ }
        else if (request.starts_with("PUT /snapshot/create ")) {
            const auto type = text(body.at("snapshot_type"));
            require(paused && (type == "Full" || (type == "Diff" && tracking)) && body.at("sync_snapshot_files").as_bool(),
                "Snapshot was not paused/synchronized or dirty tracking was disabled");
            const auto memory = fs::path(text(body.at("mem_file_path")));
            require(!fs::exists(text(body.at("snapshot_path"))) && (type == "Diff" || !fs::exists(memory)), "Snapshot state or full memory file was reused");
            write_text(text(body.at("snapshot_path")), json::serialize(Object{{"counter", state->counter.load()},
                {"allocated_mib", state->allocated_mib.load()}, {"first", state->first.load()}, {"captures", captures + (type == "Diff")}}));
            if (type == "Diff") {
                require(memory.filename() == "base.mem" && (fs::exists(memory) == (captures > 0)), "Diff did not update its cumulative base");
                make_diff_fixture(memory, ++captures);
            }
            else write_text(text(body.at("mem_file_path")), "guest ram");
            fail = fs::exists(socket_path.string() + ".fail-capture");
        } else if (request.starts_with("PUT /snapshot/load ")) {
            require(fs::is_regular_file(text(body.at("snapshot_path"))) &&
                fs::is_regular_file(text(body.at("mem_backend").as_object().at("backend_path"))) &&
                body.at("resume_vm").as_bool(), "Restore backing is missing");
            tracking = body.at("track_dirty_pages").as_bool();
            fail = fs::exists(socket_path.string() + ".fail");
            if (!fail) {
                const auto saved = json::parse(read_text(text(body.at("snapshot_path")))).as_object();
                state->counter.store(static_cast<int>(integer(saved.at("counter"))));
                state->allocated_mib.store(static_cast<int>(integer(saved.at("allocated_mib"))));
                state->first.store(static_cast<int>(integer(saved.at("first"))));
                captures = static_cast<int>(integer(saved.at("captures")));
                start_fake_guest(text(body.at("vsock_override").as_object().at("uds_path")), state);
            }
        } else throw std::runtime_error("Unexpected fake API request");
        std::ofstream trace(socket_path.string() + ".trace", std::ios::app);
        trace << json::serialize(Object{{"request", request}, {"body", body}}) << '\n';
        trace.close();
        send_all(client.get(), fail ? "HTTP/1.1 500 Failed\r\nContent-Length: 0\r\n\r\n" : "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n");
    }
}

void test_firecracker_fresh_snapshots() {
    require_socket_access();
    TemporaryDirectory temporary(fs::temp_directory_path());
    const auto executable = fs::read_symlink("/proc/self/exe");
    const auto control = temporary.path() / "parent.sock";
    ProcessManager parent;
    const auto pid = parent.launch({executable.string(), "--api-sock", control.string()}, temporary.path() / "parent.log");
    parent.wait_for_socket(control, pid);
    auto root = parent.track(0, pid, control, {});
    Options options;
    options.backend = "firecracker";
    options.snapshot_type = "full";
    options.firecracker = executable;
    options.output = temporary.path();
    auto backend = make_backend(options, temporary.path());
    require(!backend->prepare_round(root, 1).contains("prepare_ns"), "Snapshot prepared outside child timer");
    auto first = backend->clone(root, 1), second = backend->clone(root, 2);
    const auto first_memory = text(first.metadata.at("snapshot_memory_path"));
    const auto second_memory = text(second.metadata.at("snapshot_memory_path"));
    require(first_memory != second_memory && fs::exists(first_memory) && fs::exists(second_memory), "Children did not retain unique backing");
    std::istringstream trace(read_text(control.string() + ".trace"));
    std::string line;
    for (int index = 0; index < 6; ++index) {
        require(bool(std::getline(trace, line)), "Missing per-child snapshot requests");
        const auto entry = json::parse(line).as_object();
        const auto& body = entry.at("body").as_object();
        require(index % 3 == 1 ? text(entry.at("request")).starts_with("PUT /snapshot/create ") :
            text(body.at("state")) == (index % 3 == 0 ? "Paused" : "Resumed"), "Incorrect capture/resume order");
    }
    require(!std::getline(trace, line), "Unexpected round snapshot preparation");
    backend->destroy(first.child);
    require(!first.child->alive && !fs::exists(first_memory) && !fs::exists(text(first.metadata.at("snapshot_path"))) &&
        fs::exists(second_memory), "Backing released before exit or sibling backing removed");
    backend->destroy(second.child);
    backend->release_round(root);
    write_text(temporary.path() / "f3.sock.fail", "fail restore");
    bool failed = false;
    try { backend->clone(root, 3); } catch (const std::runtime_error&) { failed = true; }
    require(failed, "Failed restore accepted");
    require(backend->cleanup().empty() && !fs::exists(temporary.path() / "c3.mem") && !fs::exists(temporary.path() / "c3.state"),
        "Failed restore did not clean child and backing");
    parent.stop(root, SIGTERM);
}

void test_firecracker_diff_snapshots() {
    require_socket_access();
    TemporaryDirectory temporary(fs::temp_directory_path());
    Options options;
    options.backend = "firecracker";
    options.firecracker = fs::read_symlink("/proc/self/exe");
    options.output = temporary.path();
    auto backend = make_backend(options, temporary.path());
    auto root = backend->boot();
    backend->marker(root, 1);
    auto first = backend->clone(root, 1);
    backend->marker(root, 2);
    auto second = backend->clone(root, 2);
    const auto first_memory = text(first.metadata.at("snapshot_memory_path"));
    const auto second_memory = text(second.metadata.at("snapshot_memory_path"));
    require(integer(backend->ready(first.child).at("counter")) == 1 && integer(backend->ready(second.child).at("counter")) == 2,
        "Diff children did not inherit their own point-in-time state");
    require(read_page(first_memory, 0) == std::string(4096, 'A') && read_page(second_memory, 0) == std::string(4096, '\0') &&
        read_page(second_memory, 8192) == std::string(4096, 'P') && read_page(second_memory, 65536) == std::string(4096, 'B'),
        "Diff restore lost base pages, zero writes, or earlier child isolation");
    require(first.metadata.at("initial_base").as_bool() && !second.metadata.at("initial_base").as_bool() &&
        integer(second.metadata.at("prepare_ns")) >= integer(second.metadata.at("capture_ns")) +
            integer(second.metadata.at("backing_copy_ns")), "Diff initial-base or preparation accounting failed");
    backend->destroy(first.child);
    backend->destroy(second.child);
    backend->release_round(root);
    require(fs::exists(temporary.path() / "base.mem"), "Cumulative base lost between rounds");
    backend->marker(root, 3);
    auto third = backend->clone(root, 3);
    const auto third_memory = text(third.metadata.at("snapshot_memory_path"));
    require(!third.metadata.at("initial_base").as_bool() && read_page(third_memory, 8192) == std::string(4096, 'P') &&
        read_page(third_memory, 65536) == std::string(4096, 'B') && read_page(third_memory, 131072) == std::string(4096, 'C'),
        "Diff reconstruction did not retain history across rounds");
    write_text(temporary.path() / "f4.sock.fail", "fail restore");
    bool failed = false;
    try { backend->clone(root, 4); } catch (const std::runtime_error&) { failed = true; }
    require(failed && backend->cleanup().empty() && !root->alive && !third.child->alive &&
        !fs::exists(third_memory) && !fs::exists(temporary.path() / "base.mem") && !fs::exists(temporary.path() / "c4.mem"),
        "Diff failed-restore cleanup left a VM or backing memory");

    auto retry = make_backend(options, temporary.path());
    // The process/socket IDs are reusable after cleanup; force capture failure before child launch.
    root = retry->boot();
    write_text(root->control.string() + ".fail-capture", "fail capture");
    failed = false;
    try { retry->clone(root, 5); } catch (const std::runtime_error&) { failed = true; }
    require(failed && retry->cleanup().empty() && !fs::exists(temporary.path() / "base.mem") && !fs::exists(temporary.path() / "c5.state"),
        "Capture failure left partial diff files");
}
}


namespace vmbench::tests {
namespace {
class DepthFake final : public Backend {
public:
    VmHandle boot() override {
        auto vm = std::make_shared<Vm>(); vm->pid = 10000;
        nodes.push_back(vm); states[0] = {0, 0, 0}; return vm;
    }
    void marker(const VmHandle& vm, int value) override { states.at(vm->id).counter = value; }
    Object ready(const VmHandle& vm) override {
        require(vm->alive, "Depth fake used a dead VM");
        const auto& state = states.at(vm->id);
        return {{"counter", state.counter}, {"allocated", state.mib * 1048576}, {"first", state.first}, {"checksum", 0}};
    }
    Object command(const VmHandle& vm, const std::string& request) override {
        auto& state = states.at(vm->id);
        if (request.starts_with("ALLOC ")) { state.mib = std::stoi(request.substr(6)); state.first = 1; }
        else if (request.starts_with("WRITE ")) {
            state.first = std::stoi(request.substr(6));
            if (shared_writes && vm->id > 0) states.at(vm->id - 1).first = state.first;
        }
        auto result = ready(vm);
        if (request == "SCAN") result["checksum"] = populated_checksum(state.mib) - 1 + state.first + (bad_scan && vm->id > 0);
        return result;
    }
    Object prepare_round(const VmHandle&, int) override { throw std::runtime_error("Depth unexpectedly used batch preparation"); }
    CloneResult clone(const VmHandle& source, int id) override {
        require(source == nodes.back() && source->id == id - 1 &&
            std::all_of(nodes.begin(), nodes.end(), [](const auto& vm) { return vm->alive; }),
            "Depth did not retain ancestors or clone the immediate predecessor");
        auto vm = std::make_shared<Vm>(); vm->id = id; vm->pid = 10000 + id;
        states[id] = states.at(wrong_source ? 0 : source->id); nodes.push_back(vm);
        return {vm, {{"parent_pause_us", 10}}};
    }
    void destroy(const VmHandle& vm) override { vm->alive = false; }
    void release_round(const VmHandle&) override { throw std::runtime_error("Depth unexpectedly used batch cleanup"); }
    json::array cleanup() override { for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) destroy(*it); return {}; }
    VmList nodes;
    bool shared_writes = false, bad_scan = false, wrong_source = false;
private:
    struct State { int counter, mib, first; };
    std::map<int, State> states;
};
Object depth_sample(const MeasurementEvent& event, int64_t clock) {
    json::array processes;
    int pss = 0;
    for (const auto& vm : event.vms) {
        const int memory = 100 + vm->id * 2;
        pss += memory;
        processes.push_back(Object{{"vm_id", vm->id}, {"pid", vm->pid},
            {"memory_kib", Object{{"Pss", memory}, {"Private_Dirty", 10 + vm->id}, {"Private_Clean", 2}, {"AnonHugePages", 65536}}}});
    }
    return {{"monotonic_ns", clock}, {"phase", event.phase}, {"round", event.round}, {"position", event.position},
        {"fork_number", event.fork_number}, {"live_vms", event.vms.size()}, {"processes", processes},
        {"memory_sum_kib", Object{{"Pss", pss}}}, {"cgroup", nullptr}, {"extra", event.extra}};
}
void require_socket_access() {
    int pair[2];
    require(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0, "Cannot probe simulator socket access");
    FileDescriptor left(pair[0]), right(pair[1]);
    if (send(left.get(), "x", 1, MSG_NOSIGNAL) < 0 && errno == EPERM)
        throw Skipped{"Unix-socket sends denied by execution sandbox (EPERM); API simulator not run"};
}
}
void test_depth_lifecycle() {
    Options options; options.experiment = "depth"; options.settle_ms = 0;
    DepthFake backend;
    int ready = 0, verified = 0;
    DepthBenchmark benchmark(backend, options, [&](const MeasurementEvent& event) {
        require(event.vms.size() == static_cast<size_t>(event.position + 1), "Depth live count differs from ancestry");
        ready += event.phase == "depth_ready"; verified += event.phase == "depth_verified";
    });
    const auto result = benchmark.run();
    require(ready == 64 && verified == 64 && backend.nodes.size() == 65 && integer(result.at("maximum_live_vms")) == 65,
        "Depth-64 retained chain did not complete");
    require(backend.cleanup().empty() && std::none_of(backend.nodes.begin(), backend.nodes.end(), [](const auto& vm) { return vm->alive; }),
        "Depth fake cleanup left ancestors alive");
}
void test_depth_validation() {
    for (int failure = 0; failure < 3; ++failure) {
        Options options; options.experiment = "depth"; options.depth = 4; options.settle_ms = 0;
        DepthFake backend;
        backend.shared_writes = failure == 0; backend.bad_scan = failure == 1; backend.wrong_source = failure == 2;
        bool rejected = false;
        try { DepthBenchmark benchmark(backend, options, [](const MeasurementEvent&) {}); benchmark.run(); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "Corrupt ancestry, populated pages or shared child writes were accepted");
        backend.cleanup();
    }
}
void test_depth_cli() {
    const auto options = parse({"benchmark", "--experiment", "depth", "--depth", "64", "--populate-mib", "128", "--memory-mib", "512"});
    require(options.experiment == "depth" && options.depth == 64 && options.huge_pages == "Transparent" &&
        integer(inspect_host(options).at("total_forks")) == 64 && integer(inspect_host(options).at("maximum_live_vms")) == 65,
        "Depth CLI defaults or planned counts are wrong");
    require(parse({"benchmark", "--depth-report", "."}).command == Command::DepthReport, "Depth report requires KVM or run parsing");
    for (const auto& arguments : std::vector<std::vector<std::string>>{
        {"benchmark", "--experiment", "depth", "--depth", "65"},
        {"benchmark", "--experiment", "depth", "--depth", "0"},
        {"benchmark", "--experiment", "depth", "--populate-mib", "256"},
        {"benchmark", "--experiment", "depth", "--huge-pages", "None"},
        {"benchmark", "--experiment", "invalid"}}) {
        bool rejected = false;
        try { parse(arguments); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "Invalid Figure 4(b) parameters accepted");
    }
}
void test_depth_reporting() {
    TemporaryDirectory temporary(fs::temp_directory_path());
    Options options; options.experiment = "depth"; options.depth = 3; options.settle_ms = 0; options.huge_pages = "Transparent";
    const Object settings{{"experiment", "depth"}, {"depth", 3}, {"populate_mib", 64}, {"memory_mib", 256},
        {"vcpus", 1}, {"huge_pages", "Transparent"}, {"settle_ms", 0}, {"repetitions", 2},
        {"snapshot_type", "diff"}, {"backends", json::array{"lightning"}}};
    write_text(temporary.path() / "settings.json", json::serialize(settings));
    const auto output = temporary.path() / "lightning/rep-001";
    fs::create_directories(output);
    std::vector<Object> samples{depth_sample({"before_boot", 0, 0, 0, {}, {}}, 0)};
    DepthFake backend;
    DepthBenchmark benchmark(backend, options, [&](const MeasurementEvent& event) { samples.push_back(depth_sample(event, samples.size())); });
    const auto result = benchmark.run();
    samples.push_back(depth_sample({"all_stopped", 0, 0, 3, {}, {}}, samples.size()));
    write_text(output / "summary.json", json::serialize(Object{{"status", "ok"}, {"backend", "lightning"}, {"experiment", "depth"},
        {"cleanup_errors", json::array{}}, {"preflight", settings}, {"result", result}}));
    auto save = [&] {
        std::ostringstream data;
        for (const auto& sample : samples) data << json::serialize(sample) << '\n';
        write_text(output / "samples.jsonl", data.str());
    };
    samples.back()["cgroup"] = Object{{"memory.stat", Object{{"sec_pagetables", 4096}}}};
    save();
    fs::copy(output, temporary.path() / "lightning/rep-002", fs::copy_options::recursive);
    auto second = samples;
    for (auto& sample : second) {
        if (text(sample.at("phase")) != "depth_ready") continue;
        auto& extra = sample["extra"].as_object();
        extra["guest_ready_ns"] = integer(extra.at("guest_ready_ns")) + 2000000;
    }
    std::ostringstream second_data;
    for (const auto& sample : second) second_data << json::serialize(sample) << '\n';
    write_text(temporary.path() / "lightning/rep-002/samples.jsonl", second_data.str());
    generate_depth_report(temporary.path());
    const auto report = json::parse(read_text(temporary.path() / "comparison.json")).as_object();
    const auto& first_row = report.at("rows").as_array().front().as_object();
    require(integer(first_row.at("guest_ready_ms_samples")) == 2 &&
        first_row.at("guest_ready_ms_max").as_double() - first_row.at("guest_ready_ms_min").as_double() > 1.999 &&
        std::abs(first_row.at("guest_ready_ms_median").as_double() - first_row.at("guest_ready_ms_min").as_double() - 1.0) < 1e-9 &&
        first_row.at("guest_ready_ms_p99") == first_row.at("guest_ready_ms_max"), "Depth repetition aggregation is incorrect");
    require(report.at("rows").as_array().size() == 3 && integer(report.at("vm_runs_started")) == 0 &&
        read_text(temporary.path() / "depth.csv").find("verified_private_mib_median") != std::string::npos &&
        read_text(temporary.path() / "dashboard.html").find("AnonHugePages") != std::string::npos,
        "Depth report, separate memory phases or THP evidence missing");
    samples[4]["extra"].as_object()["source_id"] = 0; save();
    bool rejected = false;
    try { generate_depth_report(temporary.path()); } catch (const std::exception&) { rejected = true; }
    require(rejected && text(json::parse(read_text(temporary.path() / "reporting-status.json")).as_object().at("status")) == "failed",
        "Depth reporter accepted a sibling fanout as a chain");
    samples[4]["extra"].as_object()["source_id"] = 1;
    samples[4]["processes"].as_array().front().as_object()["pid"] = 9999; save(); rejected = false;
    try { generate_depth_report(temporary.path()); } catch (const std::exception&) { rejected = true; }
    require(rejected, "Depth reporter accepted replacement of a retained ancestor");
    samples[4]["processes"].as_array().front().as_object()["pid"] = backend.nodes.front()->pid;
    samples.pop_back(); save(); rejected = false;
    try { generate_depth_report(temporary.path()); } catch (const std::exception&) { rejected = true; }
    require(rejected, "Depth report accepted missing final teardown");
}
void test_firecracker_depth() {
    require_socket_access();
    for (const auto* type : {"diff", "full"}) {
        TemporaryDirectory temporary(fs::temp_directory_path());
        Options options; options.experiment = "depth"; options.depth = 3; options.populate_mib = 1; options.settle_ms = 0;
        options.snapshot_type = type; options.backend = "firecracker";
        options.firecracker = fs::read_symlink("/proc/self/exe"); options.output = temporary.path();
        auto backend = make_backend(options, temporary.path());
        int observed = 0;
        DepthBenchmark benchmark(*backend, options, [&](const MeasurementEvent& event) {
            if (event.phase != "depth_ready") return;
            require(event.extra.at("backend_metadata").as_object().at("child_track_dirty_pages").as_bool() == (options.snapshot_type == "diff"),
                "Restored depth source dirty tracking does not match snapshot mode");
            ++observed;
        });
        benchmark.run();
        require(observed == 3 && read_text(temporary.path() / "f1.sock.trace").find("PUT /snapshot/create") != std::string::npos &&
            read_text(temporary.path() / "f2.sock.trace").find("PUT /snapshot/create") != std::string::npos,
            "Firecracker depth did not snapshot restored descendants");
        require(backend->cleanup().empty() && !fs::exists(temporary.path() / "base.mem") && !fs::exists(temporary.path() / "c3.mem"),
            "Depth cleanup retained Firecracker backing");
    }
}
}

int main(int argc, char** argv) {
    using namespace vmbench::tests;
    if (argc >= 3 && std::string(argv[1]) == "--api-sock") {
        try { return fake_firecracker(argv[2]); }
        catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
    }
    const std::vector<std::pair<const char*, void(*)()>> tests = {
        {"depth-64 retained chain", test_depth_lifecycle}, {"depth inheritance/scan/isolation failures", test_depth_validation},
        {"depth CLI and THP policy", test_depth_cli}, {"saved depth reporting and rejection", test_depth_reporting},
        {"Firecracker recursive full/diff sources (simulated API)", test_firecracker_depth},
        {"2500-fork batch lifecycle", test_batch_lifecycle}, {"inheritance failure detection", test_inheritance_failure},
        {"proc parser", test_proc_parser}, {"live process collection", test_live_process_collection},
        {"minimal cgroup categories", test_cgroup_filter}, {"KVM PID matching", test_kvm_pid_matching},
        {"CSV schema", test_csv_output}, {"summary statistics", test_statistics}, {"CLI options", test_cli_options},
        {"HTTP protocol", test_http_protocol}, {"JSON round-trip", test_json_round_trip},
        {"Unix-socket framing", test_socket_framing}, {"native process cleanup", test_process_cleanup},
        {"saved-result reporting", test_saved_report}, {"incomplete report rejection", test_incomplete_report},
        {"combined creation timing", test_combined_creation_timing},
        {"Firecracker per-fork snapshots and cleanup (simulated API)", test_firecracker_fresh_snapshots},
        {"sparse backing copies and base-update isolation", test_sparse_diff_memory},
        {"Firecracker diff lifecycle and failures (simulated API)", test_firecracker_diff_snapshots}
    };
    int passed = 0, skipped = 0, failed = 0;
    for (const auto& [name, run] : tests) {
        try { run(); ++passed; std::cout << "PASS: " << name << '\n'; }
        catch (const Skipped& reason) { ++skipped; std::cout << "SKIP: " << name << ": " << reason.reason << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL: " << name << ": " << error.what() << '\n'; }
    }
    std::cout << passed << " passed; " << skipped << " skipped; " << failed << " failed. No KVM VMs run.\n";
    return failed == 0 ? 0 : 1;
}
