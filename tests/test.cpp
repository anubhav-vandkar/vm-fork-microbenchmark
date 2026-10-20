#include "benchmark.hpp"
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>



namespace vmbench::tests {
struct Skipped { std::string reason; };
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
    TemporaryDirectory temporary("/tmp");
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
    TemporaryDirectory temporary("/tmp");
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
    TemporaryDirectory temporary("/tmp");
    ProcessManager processes;
    const auto pid = processes.launch({"/bin/sleep", "30"}, temporary.path() / "process.log");
    auto vm = processes.track(1, pid, temporary.path() / "api", temporary.path() / "vsock");
    processes.stop(vm, SIGTERM);
    require(!vm->alive && vm->pidfd.get() < 0, "VM process descriptor retained after stop");
    require(processes.active().empty() && processes.launched_count() == 0, "Exited process records retained");
    processes.stop(vm, SIGTERM);
}
}

int main() {
    using namespace vmbench::tests;
    const std::vector<std::pair<const char*, void(*)()>> tests = {
        {"2500-fork batch lifecycle", test_batch_lifecycle}, {"inheritance failure detection", test_inheritance_failure},
        {"proc parser", test_proc_parser}, {"live process collection", test_live_process_collection},
        {"minimal cgroup categories", test_cgroup_filter}, {"KVM PID matching", test_kvm_pid_matching},
        {"CSV schema", test_csv_output}, {"summary statistics", test_statistics}, {"CLI options", test_cli_options},
        {"HTTP protocol", test_http_protocol}, {"JSON round-trip", test_json_round_trip},
        {"Unix-socket framing", test_socket_framing}, {"native process cleanup", test_process_cleanup}
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
