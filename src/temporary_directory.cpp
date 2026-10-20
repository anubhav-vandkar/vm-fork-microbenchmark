#include "vmbench/temporary_directory.hpp"
#include "vmbench/error.hpp"
#include <cstdlib>
#include <vector>

namespace vmbench {
TemporaryDirectory::TemporaryDirectory(const std::filesystem::path& parent) {
    const auto pattern = (parent / "lt-batch-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    const char* created = mkdtemp(name.data());
    require(created != nullptr, "Cannot create temporary directory");
    path_ = created;
}
TemporaryDirectory::~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
}
}
