#include "test_support.hpp"
#include "vmbench/csv_writer.hpp"
#include "vmbench/statistics.hpp"
#include "vmbench/temporary_directory.hpp"

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
