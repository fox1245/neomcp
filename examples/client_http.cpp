// Example 4: a client of an HTTP server (example 2), and how it finds out which revision the server speaks.
//
// The protocol has two eras. In the older one a connection opens with a handshake in which client and server agree on
// a revision, and the server keeps a session. In the newer one (2026-07-28) there is no handshake: every request names
// the revision it speaks, and the server answers each on its own. A server may serve both, as the servers of this
// library do, and a client may speak either.
//
// Client::open finds out which it is dealing with. Starting from the newest revision the client speaks, it asks the
// server which revisions it serves; a server of the older era does not know that question and the client falls back to
// the handshake. When it returns, Client::server() tells what was found: the revision and the kind of server.
//
// Usage: client_http [URL of the endpoint [bearer token]]
// The URL is http://127.0.0.1:8000/mcp without an argument. The token goes with every request, for a server such as
// example 8 that asks for one.
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <neomcp/client.h>
#include <neomcp/client/http.h>
#include <neomcp/profile.h>

#include "client_support.h"
#include "demo_types.h"

namespace {

constexpr const char* program = "client_http";

// What contact found, in words.
void describe(const neomcp::Client& client) {
    const neomcp::ServerDescription server = client.server();
    std::cout << "  revision " << server.revision << ", ";
    switch (server.kind) {
        case neomcp::ServerKind::per_request:
            std::cout << "a server that names its revision on every request (no handshake, no session)";
            break;
        case neomcp::ServerKind::handshake:
            std::cout << "a server that opened with a handshake (a session)";
            break;
    }
    std::cout << ", over " << neomcp::profile::to_string(server.transport) << '\n';
    if (server.revisions) {
        std::cout << "  its discovery result lists:";
        for (const std::string& revision : *server.revisions) std::cout << ' ' << revision;
        std::cout << '\n';
    }
}

// Opens a client with `options`, describes what it found, and adds two numbers through it.
int run(std::string_view label, const neomcp::ClientOptions& options, const neomcp::HttpEndpoint& endpoint) {
    std::cout << label << '\n';
    auto opened = neomcp::Client::open(options, endpoint);
    if (!opened) return report(program, "opening the client", opened.error());
    neomcp::Client client = std::move(opened).value();
    describe(client);

    const auto sum = client.call<std::int64_t>(add_tool_name, AddArgs{2, 3});
    if (!sum) return report(program, "calling add", sum.error());
    if (sum->is_error || !sum->value || *sum->value != 5) return unexpected(program, "2 + 3 should be 5");
    std::cout << "  2 + 3 = " << *sum->value << '\n';
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    neomcp::HttpEndpoint endpoint;
    endpoint.url = argc > 1 ? argv[1] : "http://127.0.0.1:8000/mcp";
    if (argc > 2) endpoint.authorizer = std::make_shared<neomcp::StaticBearer>(argv[2]);

    neomcp::ClientOptions options;
    options.client_info.name = "example-client";
    options.client_info.version = "1.0.0";

    // First, a client that speaks every revision the library knows. It takes the newest one the server serves.
    if (const int status = run("every revision:", options, endpoint)) return status;

    // Then a client that speaks only the revisions of the older era, which makes it open with a handshake even though
    // the server could do without. The profiles say which era a revision belongs to, so no revision is named here.
    options.profiles.clear();
    for (const std::string_view revision : neomcp::profile::builtin_revisions()) {
        const neomcp::profile::ProfilePtr profile = neomcp::profile::builtin(revision);
        if (profile->era() == neomcp::profile::Era::legacy) options.profiles.push_back(profile);
    }
    if (const int status = run("the older era only:", options, endpoint)) return status;

    // An application that opens clients to the same server again and again can keep what the first one found
    // (Client::verdict()) and give it to the next as ClientOptions::prior, which then starts without asking.

    std::cout << "client_http: ok\n";
    return 0;
}
