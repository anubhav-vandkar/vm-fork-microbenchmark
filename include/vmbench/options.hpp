#pragma once
#include "vmbench/files.hpp"
#include <iosfwd>

namespace vmbench {
enum class Command { Run, Help, Preflight, SelfTest };
struct Options {
    fs::path repo, lightning, firecracker, kernel, initrd, output, work_dir = "/tmp", cgroup;
    std::string backend, huge_pages = "None";
    int rounds = 50, per_round = 50, memory_mib = 256, vcpus = 1, settle_ms = 200;
    Command command = Command::Run;
};
Options parse_options(int argc, char** argv);
void print_help(std::ostream& output);
}
