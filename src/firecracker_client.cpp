#include "vmbench/firecracker_client.hpp"
#include "vmbench/error.hpp"
#include "vmbench/unix_socket.hpp"
#include <sstream>

namespace vmbench {
std::string make_http_request(const std::string& method, const std::string& endpoint, const Object& body) {
    const auto payload = json::serialize(body);
    return method + " " + endpoint + " HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n" + payload;
}
int http_status(const std::string& response) {
    std::istringstream input(response);
    std::string version;
    int status = 0;
    require(bool(input >> version >> status) && version.starts_with("HTTP/"), "Malformed HTTP status line");
    return status;
}
void firecracker_request(const fs::path& socket, const std::string& method, const std::string& endpoint, const Object& body) {
    auto connection = connect_unix(socket);
    send_all(connection.get(), make_http_request(method, endpoint, body));
    const auto response = read_http_headers(connection.get());
    require(http_status(response) == 204, "Firecracker " + endpoint + ": " + response);
}
}
