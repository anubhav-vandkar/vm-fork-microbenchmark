#include "test_support.hpp"
#include <iostream>
#include <utility>
#include <vector>

int main() {
    using namespace vmbench::tests;
    const std::vector<std::pair<const char*, void(*)()>> tests = {
        {"2500-fork batch lifecycle", test_batch_lifecycle}, {"inheritance failure detection", test_inheritance_failure},
        {"proc parser", test_proc_parser}, {"live process collection", test_live_process_collection},
        {"minimal cgroup categories", test_cgroup_filter}, {"KVM PID matching", test_kvm_pid_matching},
        {"CSV schema", test_csv_output}, {"summary statistics", test_statistics}, {"CLI options", test_cli_options},
        {"HTTP protocol", test_http_protocol}, {"JSON round-trip", test_json_round_trip},
        {"Unix-socket framing", test_socket_framing}, {"native process cleanup", test_process_cleanup}
    };
    int passed = 0, skipped = 0, failed = 0;
    for (const auto& [name, run] : tests) {
        try { run(); ++passed; std::cout << "PASS: " << name << '\n'; }
        catch (const Skipped& reason) { ++skipped; std::cout << "SKIP: " << name << ": " << reason.reason << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL: " << name << ": " << error.what() << '\n'; }
    }
    std::cout << passed << " passed; " << skipped << " skipped; " << failed << " failed. No KVM VMs run.\n";
    return failed == 0 ? 0 : 1;
}
