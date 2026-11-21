// Example 2: the same server as example 1, over Streamable HTTP.
//
// An HTTP server can serve many clients at once, from other programs and other machines. neomcp::run_http serves the
// endpoint http://127.0.0.1:<port>/mcp until the process gets SIGINT or SIGTERM. Both eras of the protocol are
// served on that one address, and the client finds out which one it is talking to.
//
// The port is the first argument (a number from 0 to 65535; 0 lets the system choose a free one), 8000 without one.
// Once the server listens it writes the address to standard output, as one line, so that a script (or you) can read
// where it is. Nothing is encrypted: the server listens on the loopback address only unless told otherwise, and a
// server that other machines reach belongs behind a reverse proxy that provides TLS.
//
//   ./http_server 8000
//   ./client_http http://127.0.0.1:8000/mcp
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

#include <neomcp/http_server.h>
#include <neomcp/server.h>

#include "demo_server.h"

int main(int argc, char** argv) {
    Notes notes;
    neomcp::Server server({.name = "notes-and-calc", .version = "1.0.0"});
    add_demo_features(server, notes);

    neomcp::HttpServerOptions options;
    options.port = 8000;
    if (argc > 1) {
        const std::string_view text = argv[1];
        const char* const end = text.data() + text.size();
        const auto parsed = std::from_chars(text.data(), end, options.port);
        if (parsed.ec != std::errc{} || parsed.ptr != end) {
            std::cerr << "http_server: \"" << text << "\" is not a port (0 to 65535)\n";
            return EXIT_FAILURE;
        }
    }

    // Told the port the listener is bound to, which is the one asked for unless that was 0.
    options.on_listening = [address = options.bind_address, path = options.path](std::uint16_t port) {
        std::cout << "http://" << address << ':' << port << path << std::endl;
    };
    return neomcp::run_http(server, options);
}
