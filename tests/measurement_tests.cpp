#include "test_support.hpp"
#include "vmbench/cgroup_reader.hpp"
#include "vmbench/kvm_reader.hpp"
#include "vmbench/proc_reader.hpp"
#include "vmbench/sample_collector.hpp"
#include "vmbench/temporary_directory.hpp"
#include <unistd.h>

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
