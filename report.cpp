#include "benchmark.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>



namespace vmbench {
void write_text(const fs::path& path, const std::string& contents) {
    std::ofstream output(path);
    require(bool(output), "Cannot write " + path.string());
    output << contents;
    require(bool(output), "Write failed " + path.string());
}
}

namespace vmbench {
namespace {
std::string current_memory(const Object& cgroup) {
    const auto& value = cgroup.at("memory.current");
    if (!value.is_string()) return {};
    auto result = text(value);
    result.erase(std::remove(result.begin(), result.end(), '\n'), result.end());
    return result;
}
}
CsvWriter::CsvWriter(const fs::path& path) : output_(path) {
    require(bool(output_), "Cannot open CSV output");
    output_ << "phase,round,position,fork_number,live_vms,pss_kib,rss_kib,private_dirty_kib,cgroup_current_bytes,kvm_sec_pagetables_bytes\n";
}
void CsvWriter::write(const Object& sample) {
    const auto& totals = sample.at("memory_sum_kib").as_object();
    output_ << text(sample.at("phase")) << ',' << integer(sample.at("round")) << ',' << integer(sample.at("position")) << ','
        << integer(sample.at("fork_number")) << ',' << integer(sample.at("live_vms")) << ',' << integer(totals.at("Pss")) << ','
        << integer(totals.at("Rss")) << ',' << integer(totals.at("Private_Dirty")) << ',';
    if (sample.at("cgroup").is_object()) {
        const auto& cgroup = sample.at("cgroup").as_object();
        output_ << current_memory(cgroup) << ',';
        if (const auto bytes = cgroup.at("memory.stat").as_object().if_contains("sec_pagetables")) output_ << integer(*bytes);
    } else output_ << ',';
    output_ << '\n';
    output_.flush();
    require(bool(output_), "CSV write failed");
}
}

namespace vmbench {
namespace {
json::value nullable(std::optional<int64_t> number) {
    return number ? json::value(*number) : json::value(nullptr);
}
}
void Statistics::observe(const Object& sample) {
    const auto phase = text(sample.at("phase"));
    const auto pss = integer(sample.at("memory_sum_kib").as_object().at("Pss"));
    if (phase == "root_ready") root_pss_ = pss;
    if (phase == "batch_ready" && (!peak_pss_ || pss > *peak_pss_)) peak_pss_ = pss;
    if (phase == "batch_destroyed") final_pss_ = pss;
    if (phase == "child_ready") readiness_times_.push_back(static_cast<uint64_t>(integer(sample.at("extra").as_object().at("guest_ready_ns"))));
}
Object Statistics::report() const {
    Object result{{"root_pss_kib", nullable(root_pss_)}, {"maximum_batch_pss_kib", nullable(peak_pss_)}, {"final_parent_pss_kib", nullable(final_pss_)}};
    if (readiness_times_.empty()) return result;
    auto sorted = readiness_times_;
    std::sort(sorted.begin(), sorted.end());
    result["guest_ready_median_ns"] = sorted[sorted.size() / 2];
    result["guest_ready_p95_ns"] = sorted[(sorted.size() * 95 + 99) / 100 - 1];
    return result;
}
}

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
