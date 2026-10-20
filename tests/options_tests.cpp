#include "test_support.hpp"
#include "vmbench/options.hpp"
#include <vector>

namespace vmbench::tests {
namespace {
Options parse(std::vector<std::string> arguments) {
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    return parse_options(static_cast<int>(argv.size()), argv.data());
}
}
void test_cli_options() {
    const auto options = parse({"benchmark", "--preflight", "--rounds", "4", "--forks-per-round", "3"});
    require(options.command == Command::Preflight && options.rounds == 4 && options.per_round == 3, "CLI options changed");
    try { parse({"benchmark", "--rounds", "4invalid"}); }
    catch (const std::runtime_error&) { return; }
    throw std::runtime_error("Malformed integer option accepted");
}
}
