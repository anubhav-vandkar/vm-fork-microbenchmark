#include "test_support.hpp"
#include "vmbench/file_descriptor.hpp"
#include "vmbench/firecracker_client.hpp"
#include "vmbench/json.hpp"
#include "vmbench/unix_socket.hpp"
#include <cerrno>
#include <sys/socket.h>

namespace vmbench::tests {
void test_http_protocol() {
    const auto request = make_http_request("PUT", "/snapshot/load", {{"resume_vm", true}});
    require(request.starts_with("PUT /snapshot/load HTTP/1.1\r\n"), "HTTP request line changed");
    const auto payload = json::serialize(Object{{"resume_vm", true}});
    require(request.ends_with("\r\n\r\n" + payload), "HTTP JSON body changed");
    require(request.find("Content-Length: " + std::to_string(payload.size())) != std::string::npos, "HTTP content length incorrect");
    require(http_status("HTTP/1.1 204 No Content\r\n\r\n") == 204, "HTTP status parser incorrect");
}
void test_socket_framing() {
    int pair[2];
    require(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0, "socketpair failed");
    FileDescriptor sender(pair[0]), receiver(pair[1]);
    const Object original{{"ok", true}, {"path", "/tmp/a\"b"}};
    const auto payload = json::serialize(original) + "\n";
    const auto sent = send(sender.get(), payload.data(), payload.size(), MSG_NOSIGNAL);
    if (sent < 0 && errno == EPERM) throw Skipped{"Unix-socket send denied by execution sandbox (EPERM)"};
    require(sent == static_cast<ssize_t>(payload.size()), "Socket framing write failed");
    require(json::parse(read_line(receiver.get())).as_object() == original, "Socket JSON framing failed");
}
void test_json_round_trip() {
    const auto original = json::parse("{\"ok\":true,\"path\":\"/tmp/a\\\"b\"}");
    require(json::parse(json::serialize(original)) == original, "JSON round-trip failed");
}
}
