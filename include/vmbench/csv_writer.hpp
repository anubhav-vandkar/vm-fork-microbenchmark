#pragma once
#include "vmbench/files.hpp"
#include <fstream>

namespace vmbench {
class CsvWriter {
public:
    explicit CsvWriter(const fs::path& path);
    void write(const Object& sample);
private:
    std::ofstream output_;
};
}
