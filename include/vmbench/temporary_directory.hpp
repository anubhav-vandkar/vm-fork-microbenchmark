#pragma once
#include <filesystem>

namespace vmbench {
class TemporaryDirectory {
public:
    explicit TemporaryDirectory(const std::filesystem::path& parent);
    ~TemporaryDirectory();
    const std::filesystem::path& path() const { return path_; }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
private:
    std::filesystem::path path_;
};
}
