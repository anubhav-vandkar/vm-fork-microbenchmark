#include "vmbench/statistics.hpp"
#include <algorithm>

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
