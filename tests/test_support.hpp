#pragma once
#include "vmbench/error.hpp"
#include <string>

namespace vmbench::tests {
struct Skipped { std::string reason; };
void test_batch_lifecycle();
void test_inheritance_failure();
void test_proc_parser();
void test_live_process_collection();
void test_cgroup_filter();
void test_kvm_pid_matching();
void test_csv_output();
void test_statistics();
void test_cli_options();
void test_http_protocol();
void test_socket_framing();
void test_process_cleanup();
void test_json_round_trip();
}
