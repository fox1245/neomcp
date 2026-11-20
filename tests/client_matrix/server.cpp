// The server of the client matrix as a program of its own (registrations.h), over one transport, serving only the
// revisions named on its command line (every built-in one when none is named).
//
//   neomcp_client_matrix_server stdio [revision...]
//   neomcp_client_matrix_server http [revision...]   Streamable HTTP on 127.0.0.1, path /mcp
//   neomcp_client_matrix_server sse [revision...]    the transport of events over HTTP on 127.0.0.1, path /sse
//
// Over HTTP the port is chosen by the operating system and written to standard error on a line of its own before
// anything else, and the server runs until SIGINT or SIGTERM.
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>

#include "http_sse_server.h"
#include "neomcp/http_server.h"
#include "neomcp/server.h"
#include "neomcp/stdio.h"
#include "registrations.h"

namespace {

int usage() {
    std::fputs("usage: neomcp_client_matrix_server stdio|http|sse [revision...]\n", stderr);
    return 2;
}

void announce(std::uint16_t port) {
    std::fprintf(stderr, "%u\n", static_cast<unsigned>(port));
    std::fflush(stderr);
}

int serve_sse(neomcp::Server& server) {
    http_sse_test::HttpSseServerOptions options;
    options.path = "/sse";
    try {
        http_sse_test::HttpSseServer sse(server, std::move(options));
        announce(sse.port());
        asio::io_context io;
        asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([](const auto&, int) {});
        io.run();
        sse.stop();
    } catch (const std::exception&) {
        std::fputs("neomcp_client_matrix_server: the server could not start\n", stderr);
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string_view mode = argv[1];
    std::vector<std::string> revisions;
    for (int at = 2; at < argc; ++at) revisions.emplace_back(argv[at]);
    auto profiles = client_matrix::profiles_of(revisions);
    if (profiles.size() != revisions.size()) {
        std::fputs("neomcp_client_matrix_server: no built-in profile of that revision\n", stderr);
        return 2;
    }

    neomcp::Server server(client_matrix::server_options(std::move(profiles)));
    client_matrix::register_matrix(server);

    if (mode == "stdio") return neomcp::run_stdio(server);
    if (mode == "sse") return serve_sse(server);
    if (mode != "http") return usage();
    neomcp::HttpServerOptions options;
    options.diagnostics = false;
    options.on_listening = [](std::uint16_t port) { announce(port); };
    const int code = neomcp::run_http(server, options);
    if (code != 0) std::fputs("neomcp_client_matrix_server: the server could not start\n", stderr);
    return code;
}
