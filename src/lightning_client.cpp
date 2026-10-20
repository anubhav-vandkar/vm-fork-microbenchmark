#include "vmbench/lightning_client.hpp"
#include "vmbench/error.hpp"
#include "vmbench/unix_socket.hpp"

namespace vmbench {
Object lightning_request(const fs::path& socket, const Object& request) {
    auto connection = connect_unix(socket);
    send_all(connection.get(), json::serialize(request) + "\n");
    auto response = json::parse(read_line(connection.get())).as_object();
    require(response.contains("ok") && response.at("ok").as_bool(), "Lightning error: " + json::serialize(response));
    return response;
}
}
