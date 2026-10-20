#include "vmbench/files.hpp"
#include "vmbench/error.hpp"
#include <fstream>
#include <iterator>

namespace vmbench {
std::string read_text(const fs::path& path) {
    std::ifstream input(path);
    require(bool(input), "Cannot read " + path.string());
    return {std::istreambuf_iterator<char>(input), {}};
}
json::value optional_text(const fs::path& path) {
    try {
        return json::value(read_text(path));
    } catch (const std::exception& error) {
        return Object{{"unavailable", error.what()}};
    }
}
void write_text(const fs::path& path, const std::string& contents) {
    std::ofstream output(path);
    require(bool(output), "Cannot write " + path.string());
    output << contents;
    require(bool(output), "Write failed " + path.string());
}
}
