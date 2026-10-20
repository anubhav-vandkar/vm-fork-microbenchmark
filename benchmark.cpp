#include "benchmark.hpp"



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
