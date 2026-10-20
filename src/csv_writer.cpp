#include "vmbench/csv_writer.hpp"
#include "vmbench/error.hpp"
#include <algorithm>

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
