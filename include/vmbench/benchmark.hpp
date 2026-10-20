#pragma once
#include "vmbench/backend.hpp"
#include <functional>

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
