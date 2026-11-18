// The server the official conformance runner tests: Streamable HTTP on 127.0.0.1, path /mcp, both eras on the one
// endpoint, with what the runner's scenarios call and read (everything.h): tools, resources, a resource template,
// prompts and a completer.
//
//   neomcp_conformance_fixture [port]
//
// The port defaults to 0, which picks a free one; the chosen port is then written to standard error on a line of its
// own before anything else, so that a script can read it. The server runs until SIGINT or SIGTERM.
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <system_error>

#include "everything.h"
#include "neomcp/http_server.h"
#include "neomcp/server.h"

namespace {

bool read_port(const char* argument, std::uint16_t& port) {
    const char* end = argument + std::strlen(argument);
    const auto [stop, error] = std::from_chars(argument, end, port);
    return error == std::errc() && stop == end;
}

}  // namespace

int main(int argc, char** argv) {
    std::uint16_t port = 0;
    if (argc > 2 || (argc == 2 && !read_port(argv[1], port))) {
        std::fputs("usage: neomcp_conformance_fixture [port]\n", stderr);
        return 2;
    }

    neomcp::Server server(conformance::server_options());
    conformance::register_everything(server);

    neomcp::HttpServerOptions options;
    options.port = port;
    options.diagnostics = false;
    // Every open stream holds a worker, and a scenario that fails half way may leave its session's stream open until
    // the runner exits; one run of each revision opens a few dozen sessions.
    options.max_worker_threads = 128;
    const bool announce = port == 0;
    options.on_listening = [announce](std::uint16_t bound) {
        if (!announce) return;
        std::fprintf(stderr, "%u\n", static_cast<unsigned>(bound));
        std::fflush(stderr);
    };
    const int code = neomcp::run_http(server, options);
    if (code != 0) std::fputs("neomcp_conformance_fixture: the server could not start\n", stderr);
    return code;
}
