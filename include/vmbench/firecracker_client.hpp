#pragma once
#include "vmbench/files.hpp"

namespace vmbench {
std::string make_http_request(const std::string& method, const std::string& endpoint, const Object& body);
int http_status(const std::string& response);
void firecracker_request(const fs::path& socket, const std::string& method, const std::string& endpoint, const Object& body);
}
