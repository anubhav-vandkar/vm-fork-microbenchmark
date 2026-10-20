#pragma once
#include "vmbench/json.hpp"
#include <optional>
#include <vector>

namespace vmbench {
class Statistics {
public:
    void observe(const Object& sample);
    Object report() const;
private:
    std::vector<uint64_t> readiness_times_;
    std::optional<int64_t> root_pss_, peak_pss_, final_pss_;
};
}
