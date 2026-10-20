#include "vmbench/recorder.hpp"
#include "vmbench/error.hpp"
#include <iostream>

namespace vmbench {
Recorder::Recorder(const Options& options)
    : options_(options), collector_(options.cgroup), raw_(options.output / "samples.jsonl"), csv_(options.output / "samples.csv") {
    require(bool(raw_), "Cannot open raw measurement output");
}
Object Recorder::collect_event(const MeasurementEvent& event) const {
    auto sample = collector_.collect(event.vms, event.phase != "child_ready");
    sample["phase"] = event.phase;
    sample["round"] = event.round;
    sample["position"] = event.position;
    sample["fork_number"] = event.fork_number;
    sample["extra"] = event.extra;
    return sample;
}
void Recorder::write_json(const Object& sample) {
    raw_ << json::serialize(sample) << '\n';
    raw_.flush();
    require(bool(raw_), "Raw measurement write failed");
}
void Recorder::print_progress(const MeasurementEvent& event, const Object& sample) const {
    if (event.phase == "child_ready" && event.position != options_.per_round) return;
    std::cout << event.phase << " round=" << event.round << " forks=" << event.fork_number << " live=" << event.vms.size()
        << " PSS_KiB=" << integer(sample.at("memory_sum_kib").as_object().at("Pss")) << std::endl;
}
void Recorder::record(const MeasurementEvent& event) {
    const auto sample = collect_event(event);
    write_json(sample);
    csv_.write(sample);
    statistics_.observe(sample);
    print_progress(event, sample);
}
}
