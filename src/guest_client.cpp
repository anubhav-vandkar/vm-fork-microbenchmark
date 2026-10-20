#include "vmbench/guest_client.hpp"
#include "vmbench/error.hpp"
#include "vmbench/signals.hpp"
#include "vmbench/timing.hpp"
#include "vmbench/unix_socket.hpp"

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
