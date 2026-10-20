#include "benchmark.hpp"
#include <array>
#include <boost/json/src.hpp>
#include <cerrno>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>



namespace vmbench {
int64_t integer(const json::value& value) {
    if (value.is_int64()) return value.as_int64();
    if (value.is_uint64() && value.as_uint64() <= uint64_t(std::numeric_limits<int64_t>::max())) {
        return static_cast<int64_t>(value.as_uint64());
    }
    throw std::runtime_error("Expected JSON integer within signed 64-bit range");
}
std::string text(const json::value& value) {
    return std::string(value.as_string());
}
}

namespace vmbench {
namespace {
constexpr size_t maximum_response_bytes = 1024 * 1024;
void set_socket_timeouts(int descriptor) {
    const timeval timeout{15, 0};
    require(setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0, "Cannot set receive timeout");
    require(setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0, "Cannot set send timeout");
}
ssize_t receive(int descriptor, char* buffer, size_t size) {
    for (;;) {
        const auto received = recv(descriptor, buffer, size, 0);
        if (received < 0 && errno == EINTR) { check_interrupted(); continue; }
        require(received > 0, "Socket response ended or timed out");
        return received;
    }
}
}
FileDescriptor connect_unix(const fs::path& path) {
    FileDescriptor descriptor(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    require(descriptor.get() >= 0, "Cannot create socket");
    set_socket_timeouts(descriptor.get());
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto name = path.string();
    require(name.size() < sizeof(address.sun_path), "UNIX socket path too long: " + name);
    std::memcpy(address.sun_path, name.c_str(), name.size() + 1);
    require(connect(descriptor.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "Connect " + name + ": " + std::strerror(errno));
    return descriptor;
}
void send_all(int descriptor, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const auto count = send(descriptor, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) { check_interrupted(); continue; }
        require(count > 0, "Socket write failed: " + std::string(std::strerror(errno)));
        sent += static_cast<size_t>(count);
    }
}
std::string read_line(int descriptor) {
    std::string result;
    while (result.size() < maximum_response_bytes) {
        char character;
        receive(descriptor, &character, 1);
        if (character == '\n') return result;
        result += character;
    }
    throw std::runtime_error("Socket response too large");
}
std::string read_http_headers(int descriptor) {
    std::string result;
    std::array<char, 4096> buffer{};
    while (result.find("\r\n\r\n") == std::string::npos) {
        const auto count = receive(descriptor, buffer.data(), buffer.size());
        result.append(buffer.data(), static_cast<size_t>(count));
        require(result.size() <= maximum_response_bytes, "HTTP response too large");
    }
    return result;
}
}

namespace vmbench {
Object lightning_request(const fs::path& socket, const Object& request) {
    auto connection = connect_unix(socket);
    send_all(connection.get(), json::serialize(request) + "\n");
    auto response = json::parse(read_line(connection.get())).as_object();
    require(response.contains("ok") && response.at("ok").as_bool(), "Lightning error: " + json::serialize(response));
    return response;
}
}

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

namespace vmbench {
namespace {
Object request_once(const fs::path& socket, const std::string& command) {
    auto connection = connect_unix(socket);
    send_all(connection.get(), "CONNECT 5000\n");
    require(read_line(connection.get()).starts_with("OK "), "Vsock handshake failed");
    send_all(connection.get(), command + "\n");
    return json::parse(read_line(connection.get())).as_object();
}
}
Object guest_request(const fs::path& socket, const std::string& command) {
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    std::string last_error;
    do {
        check_interrupted();
        try { return request_once(socket, command); }
        catch (const std::exception& error) { last_error = error.what(); sleep_ms(2); }
    } while (Clock::now() < deadline);
    throw std::runtime_error("Guest did not respond: " + last_error);
}
}
