#pragma once
#include "vmbench/file_descriptor.hpp"
#include "vmbench/files.hpp"

namespace vmbench {
FileDescriptor connect_unix(const fs::path& path);
void send_all(int descriptor, const std::string& data);
std::string read_line(int descriptor);
std::string read_http_headers(int descriptor);
}
