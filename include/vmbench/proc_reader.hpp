#pragma once
#include "vmbench/files.hpp"
#include "vmbench/vm.hpp"

namespace vmbench {
Object parse_numeric_lines(const std::string& input, bool colon_delimited);
Object read_process_metrics(const Vm& vm);
Object sum_process_memory(const json::array& processes);
}
