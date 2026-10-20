#pragma once
#include <boost/json.hpp>
#include <cstdint>
#include <string>

namespace vmbench {
namespace json = boost::json;
using Object = json::object;
int64_t integer(const json::value& value);
std::string text(const json::value& value);
}
