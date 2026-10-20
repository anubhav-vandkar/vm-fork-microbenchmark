#include "vmbench/unix_socket.hpp"
#include "vmbench/error.hpp"
#include "vmbench/signals.hpp"
#include <array>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>

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
