// An HTTP server on a loopback port the system chooses (neomcp::http) and a client of it (neomcp::client_http).
#include <iostream>
#include <string>
#include <utility>

#include <neomcp/client.h>
#include <neomcp/client/http.h>
#include <neomcp/http_server.h>
#include <neomcp/server.h>

#include "add.h"
#include "call.h"

int main() {
    neomcp::Server server({.name = "installed-http", .version = "1.0.0"});
    add_tool(server);
    const neomcp::HttpServerOptions options;
    neomcp::HttpServer http(server, options);
    const auto port = http.start();
    if (!port) {
        std::cerr << "http: the server did not start: " << port.error << "\n";
        return 1;
    }
    neomcp::HttpEndpoint endpoint;
    endpoint.url = "http://127.0.0.1:" + std::to_string(*port.value) + options.path;
    return call_add("http", neomcp::Client::open({}, std::move(endpoint)));
}
