#include "benchmark.hpp"
#include <thread>



namespace vmbench {
Benchmark::Benchmark(Backend& backend, const Options& options, Observer observer)
    : backend_(backend), options_(options), observer_(std::move(observer)) {}
void Benchmark::emit(const std::string& phase, int round, int position, Object extra) {
    observer_({phase, round, position, completed_, active_, std::move(extra)});
}
void Benchmark::verify_state(const Object& state, int round) const {
    require(integer(state.at("counter")) == round && integer(state.at("allocated")) == 0,
        "Wrong inherited state or unexpected memory workload");
}
void Benchmark::create_child(int round, int position) {
    check_interrupted();
    const auto started = now_ns();
    auto cloned = backend_.clone(active_.front(), completed_ + 1);
    const auto returned = now_ns();
    const auto state = backend_.ready(cloned.child);
    const auto ready = now_ns();
    verify_state(state, round);
    active_.push_back(cloned.child);
    ++completed_;
    sleep_ms(options_.settle_ms);
    emit("child_ready", round, position, {{"fork_return_ns", returned - started},
        {"guest_ready_ns", ready - started}, {"backend_metadata", std::move(cloned.metadata)}});
}
void Benchmark::destroy_children() {
    for (size_t remaining = active_.size(); remaining > 1; --remaining) backend_.destroy(active_[remaining - 1]);
    active_.resize(1);
    backend_.release_round(active_.front());
}
void Benchmark::run_round(int round) {
    check_interrupted();
    backend_.marker(active_.front(), round);
    emit("round_before", round, 0);
    auto preparation = backend_.prepare_round(active_.front(), round);
    for (int position = 1; position <= options_.per_round; ++position) create_child(round, position);
    emit("batch_ready", round, options_.per_round, {{"preparation", std::move(preparation)}});
    const auto cleanup_started = now_ns();
    destroy_children();
    verify_state(backend_.ready(active_.front()), round);
    sleep_ms(options_.settle_ms);
    emit("batch_destroyed", round, 0, {{"destroy_batch_ns", now_ns() - cleanup_started}});
}
Object Benchmark::run() {
    active_ = {backend_.boot()};
    emit("root_ready", 0, 0);
    for (int round = 1; round <= options_.rounds; ++round) run_round(round);
    return {{"rounds_completed", options_.rounds}, {"forks_completed", completed_},
        {"vms_created", completed_ + 1}, {"maximum_live_vms", options_.per_round + 1}, {"final_live_vms", 1}};
}
}


namespace vmbench {
uint64_t populated_checksum(int memory_mib) {
    const auto pages = static_cast<uint64_t>(memory_mib) * 1024 * 1024 / 4096;
    const auto remainder = pages % 251;
    return (pages / 251) * (251 * 252 / 2) + remainder * (remainder + 1) / 2;
}
namespace {
int source_byte(int depth) { return depth % 251 + 1; }
int child_byte(int depth) { return (depth + 127) % 251 + 1; }
void verify_guest_state(const Object& state, int populated_mib, int counter, int first) {
    require(integer(state.at("allocated")) == static_cast<int64_t>(populated_mib) * 1024 * 1024 &&
        integer(state.at("counter")) == counter && integer(state.at("first")) == first,
        "Guest inheritance or write-isolation check failed");
}
void verify_guest_buffer(Backend& backend, const VmHandle& vm, int populated_mib, int counter, int first) {
    const auto scan = backend.command(vm, "SCAN");
    verify_guest_state(scan, populated_mib, counter, first);
    require(static_cast<uint64_t>(integer(scan.at("checksum"))) == populated_checksum(populated_mib) - 1 + first,
        "Guest populated-page checksum differs from the expected pattern");
}
void populate_root(Backend& backend, const VmHandle& root, int populated_mib) {
    backend.command(root, "ALLOC " + std::to_string(populated_mib));
    backend.marker(root, 0);
    verify_guest_buffer(backend, root, populated_mib, 0, 1);
}
void record_cxl_usage(Backend& backend, const Options& options, Object& extra, const VmHandle& vm, bool before_fork) {
    if (options.backend != "lightning-cxl") return;
    auto usage = backend.cxl_usage(vm);
    validate_cxl_usage(usage, options.memory_mib, before_fork);
    extra["cxl"] = std::move(usage);
}
}
DepthBenchmark::DepthBenchmark(Backend& backend, const Options& options, Observer observer)
    : backend_(backend), options_(options), observer_(std::move(observer)) {}
void DepthBenchmark::emit(const std::string& phase, int depth, Object extra) {
    observer_({phase, 0, depth, depth, chain_, std::move(extra)});
}
void DepthBenchmark::initialize_root() {
    chain_ = {backend_.boot()};
    populate_root(backend_, chain_.front(), options_.populate_mib);
    sleep_ms(options_.settle_ms);
    Object extra{{"population_verified", true}};
    record_cxl_usage(backend_, options_, extra, chain_.front(), true);
    emit("root_ready", 0, std::move(extra));
}
void DepthBenchmark::verify_private_write(const VmHandle& source, const VmHandle& child, int depth) {
    const auto scan = backend_.command(child, "SCAN");
    require(static_cast<uint64_t>(integer(scan.at("checksum"))) ==
        populated_checksum(options_.populate_mib) - 1 + source_byte(depth), "Inherited populated-page checksum failed");
    backend_.command(child, "WRITE " + std::to_string(child_byte(depth)));
    backend_.marker(child, 100000 + depth);
    verify_guest_state(backend_.ready(child), options_.populate_mib, 100000 + depth, child_byte(depth));
    verify_guest_state(backend_.ready(source), options_.populate_mib, depth, source_byte(depth));
}
void DepthBenchmark::fork_generation(int depth) {
    check_interrupted();
    const auto source = chain_.back();
    backend_.marker(source, depth);
    verify_guest_state(backend_.command(source, "WRITE " + std::to_string(source_byte(depth))),
        options_.populate_mib, depth, source_byte(depth));
    const auto started = now_ns();
    auto cloned = backend_.clone(source, depth);
    const auto returned = now_ns();
    const auto state = backend_.ready(cloned.child);
    const auto ready = now_ns();
    verify_guest_state(state, options_.populate_mib, depth, source_byte(depth));
    chain_.push_back(cloned.child);
    if (options_.backend == "lightning-cxl") validate_cxl_fork(cloned.metadata, options_.memory_mib, depth);
    Object extra{{"depth", depth}, {"source_id", source->id}, {"child_id", cloned.child->id},
        {"fork_return_ns", returned - started}, {"guest_ready_ns", ready - started},
        {"backend_metadata", std::move(cloned.metadata)}, {"inheritance_verified", true}};
    record_cxl_usage(backend_, options_, extra, cloned.child, false);
    emit("depth_ready", depth, std::move(extra));
    verify_private_write(source, cloned.child, depth);
    sleep_ms(options_.settle_ms);
    Object verified{{"depth", depth}, {"scan_verified", true}, {"isolation_verified", true}};
    record_cxl_usage(backend_, options_, verified, cloned.child, false);
    emit("depth_verified", depth, std::move(verified));
}
void DepthBenchmark::verify_lineage() {
    for (size_t index = 0; index < chain_.size(); ++index) {
        const bool newest = index + 1 == chain_.size();
        const auto generation = static_cast<int>(index) + 1;
        verify_guest_state(backend_.ready(chain_[index]), options_.populate_mib,
            newest ? 100000 + options_.depth : generation,
            newest ? child_byte(options_.depth) : source_byte(generation));
    }
    emit("lineage_verified", options_.depth, {{"ancestors_retained", true}});
}
Object DepthBenchmark::run() {
    initialize_root();
    for (int depth = 1; depth <= options_.depth; ++depth) fork_generation(depth);
    verify_lineage();
    return {{"depth_completed", options_.depth}, {"forks_completed", options_.depth},
        {"vms_created", options_.depth + 1}, {"maximum_live_vms", chain_.size()},
        {"final_live_vms", chain_.size()}, {"ancestors_retained", true}};
}

FootprintBenchmark::FootprintBenchmark(Backend& backend, const Options& options, Observer observer)
    : backend_(backend), options_(options), observer_(std::move(observer)) {}
void FootprintBenchmark::fork_child(int id) {
    check_interrupted();
    const auto start = now_ns();
    auto cloned = backend_.clone(family_.front(), id);
    const auto returned = now_ns();
    const auto state = backend_.ready(cloned.child);
    const auto ready = now_ns();
    verify_guest_state(state, options_.populate_mib, 0, 1);
    verify_guest_buffer(backend_, cloned.child, options_.populate_mib, 0, 1);
    family_.push_back(cloned.child);
    if (options_.backend == "lightning-cxl") validate_cxl_fork(cloned.metadata, options_.memory_mib, id);
    Object extra{{"source_id", 0}, {"child_id", id}, {"inheritance_verified", true},
        {"fork_return_ns", returned - start}, {"guest_ready_ns", ready - start},
        {"backend_metadata", std::move(cloned.metadata)}};
    record_cxl_usage(backend_, options_, extra, cloned.child, false);
    observer_({"sibling_ready", 0, id, id, family_, std::move(extra)});
}
void FootprintBenchmark::verify_family() {
    for (const auto& vm : family_) {
        check_interrupted();
        verify_guest_state(backend_.ready(vm), options_.populate_mib, vm->id, vm->id == 0 ? 1 : child_byte(vm->id));
    }
}
void FootprintBenchmark::sample_memory() {
    const auto start = Clock::now();
    for (int second = 1; second <= options_.duration_s; ++second) {
        std::this_thread::sleep_until(start + std::chrono::seconds(second));
        check_interrupted();
        Object extra;
        if (options_.backend == "lightning-cxl") {
            json::array usage;
            for (const auto& vm : family_) {
                if (vm->id == 0) continue;
                auto vm_usage = backend_.cxl_usage(vm);
                validate_cxl_usage(vm_usage, options_.memory_mib, false);
                vm_usage["vm_id"] = vm->id;
                usage.emplace_back(std::move(vm_usage));
            }
            extra["cxl_children"] = std::move(usage);
        }
        const auto elapsed = Clock::now() - start;
        require(elapsed < std::chrono::seconds(second + 1), "Footprint sampling missed its one-second window");
        extra["elapsed_ns"] = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
        observer_({"footprint_sample", 0, second, options_.children, family_, std::move(extra)});
    }
}
Object FootprintBenchmark::run() {
    family_ = {backend_.boot()};
    populate_root(backend_, family_.front(), options_.populate_mib);
    Object root{{"population_verified", true}};
    record_cxl_usage(backend_, options_, root, family_.front(), true);
    observer_({"root_ready", 0, 0, 0, family_, std::move(root)});
    for (int id = 1; id <= options_.children; ++id) fork_child(id);
    for (const auto& vm : family_) {
        if (vm->id == 0) continue;
        check_interrupted();
        backend_.command(vm, "WRITE " + std::to_string(child_byte(vm->id)));
        backend_.marker(vm, vm->id);
    }
    verify_family();
    sleep_ms(options_.settle_ms);
    sample_memory();
    verify_family();
    observer_({"footprint_verified", 0, 0, options_.children, family_, {{"isolation_verified", true}}});
    return {{"forks_completed", options_.children}, {"samples_completed", options_.duration_s},
        {"vms_created", family_.size()}, {"maximum_live_vms", family_.size()}, {"final_live_vms", family_.size()}};
}
}
