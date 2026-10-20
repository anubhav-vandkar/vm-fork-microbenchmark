#include "vmbench/options.hpp"
#include "vmbench/error.hpp"
#include <charconv>
#include <cstdlib>
#include <ostream>

namespace vmbench {
namespace {
int parse_integer(const std::string& value) {
    int result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    require(error == std::errc{} && end == value.data() + value.size(), "Invalid integer: " + value);
    return result;
}
void assign_option(Options& options, const std::string& key, const std::string& value) {
    if (key == "--repo") options.repo = value;
    else if (key == "--backend") options.backend = value;
    else if (key == "--lightning") options.lightning = value;
    else if (key == "--firecracker") options.firecracker = value;
    else if (key == "--kernel") options.kernel = value;
    else if (key == "--initrd") options.initrd = value;
    else if (key == "--output") options.output = value;
    else if (key == "--rounds") options.rounds = parse_integer(value);
    else if (key == "--forks-per-round") options.per_round = parse_integer(value);
    else if (key == "--memory-mib") options.memory_mib = parse_integer(value);
    else if (key == "--vcpus") options.vcpus = parse_integer(value);
    else if (key == "--settle-ms") options.settle_ms = parse_integer(value);
    else if (key == "--huge-pages") options.huge_pages = value;
    else if (key == "--work-dir") options.work_dir = value;
    else if (key == "--cgroup-dir") options.cgroup = value;
    else throw std::runtime_error("Unknown option " + key);
}
void validate_options(const Options& options) {
    require(options.rounds > 0 && options.rounds <= 1000, "Invalid round count");
    require(options.per_round > 0 && options.per_round <= 1000, "Invalid batch size");
    require(options.vcpus > 0 && options.vcpus <= 32, "Invalid vCPU count");
    require(options.memory_mib >= 128, "Guest capacity must be at least 128 MiB");
    require(options.settle_ms >= 0 && options.settle_ms <= 10000, "Invalid settling delay");
    require(options.huge_pages == "None" || options.huge_pages == "Transparent", "Invalid hugepage policy");
}
void resolve_paths(Options& options) {
    options.repo = fs::absolute(options.repo);
    if (options.lightning.empty()) options.lightning = options.repo / "build/cargo_target/release/lightning";
    if (options.firecracker.empty()) options.firecracker = options.repo / "build/cargo_target/release/firecracker";
    for (auto* path : {&options.lightning, &options.firecracker, &options.kernel, &options.initrd, &options.output, &options.work_dir, &options.cgroup}) {
        if (!path->empty()) *path = fs::absolute(*path);
    }
}
}
Options parse_options(int argc, char** argv) {
    Options options;
    const char* home = std::getenv("HOME");
    options.repo = fs::path(home ? home : ".") / "Projects/lightning-main";
    for (int index = 1; index < argc; ++index) {
        const std::string key = argv[index];
        if (key == "--help") { options.command = Command::Help; return options; }
        if (key == "--self-test") { options.command = Command::SelfTest; return options; }
        if (key == "--preflight") { options.command = Command::Preflight; continue; }
        require(index + 1 < argc, "Missing value for " + key);
        assign_option(options, key, argv[++index]);
    }
    validate_options(options);
    resolve_paths(options);
    return options;
}
void print_help(std::ostream& output) {
    output << "vm-fork-bench --backend lightning|firecracker --kernel VMLINUX --initrd INITRD --output NEW_DIR\n"
              "  [--repo DIR] [--lightning BIN] [--firecracker BIN] [--rounds 50] [--forks-per-round 50]\n"
              "  [--memory-mib 256] [--vcpus 1] [--huge-pages None|Transparent] [--settle-ms 200]\n"
              "  [--work-dir /tmp] [--cgroup-dir DIR] [--preflight] | --self-test\n";
}
}
