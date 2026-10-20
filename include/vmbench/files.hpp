#pragma once
#include "vmbench/json.hpp"
#include <filesystem>
#include <string>

namespace vmbench {
namespace fs = std::filesystem;
std::string read_text(const fs::path& path);
json::value optional_text(const fs::path& path);
void write_text(const fs::path& path, const std::string& contents);
}
