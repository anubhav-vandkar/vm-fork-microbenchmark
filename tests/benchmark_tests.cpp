#include "test_support.hpp"
#include "fake_backend.hpp"
#include "vmbench/benchmark.hpp"
#include <algorithm>

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
