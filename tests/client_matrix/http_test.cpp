// The client matrix over HTTP: the library's Streamable HTTP server, and the test-only server of the transport of
// events over HTTP (tests/support/http_sse_server.h), on loopback ports the operating system chooses, each serving
// what registrations.h registers; the Client runs the flow of flow.h against them. Streamable HTTP once for every
// revision that defines it and once with every revision enabled against servers that serve only the revisions with a
// handshake, only the ones that name their revision per request, and both; the transport of events once for every
// revision that defines it and once with every revision enabled.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "flow.h"
#include "http_sse_server.h"
#include "neomcp/client.h"
#include "neomcp/client/http.h"
#include "neomcp/http_server.h"
#include "neomcp/server.h"
#include "registrations.h"

namespace {

using client_matrix::Expected;
using neomcp::profile::Era;
using neomcp::profile::Transport;

struct Cell {
    std::string name;
    Transport transport = Transport::streamable_http;
    std::vector<std::string> client;  // the revisions the client may speak; empty: all
    std::vector<std::string> server;  // the revisions the server serves; empty: all
};

std::vector<Cell> cells() {
    std::vector<Cell> found;
    for (const std::string& revision : client_matrix::revisions_defining(Transport::streamable_http)) {
        found.push_back(Cell{"StreamableHttp_Revision_" + client_matrix::test_name(revision),
                             Transport::streamable_http, {revision}, {}});
    }
    found.push_back(Cell{"StreamableHttp_AllRevisions_ServerWithAHandshakeOnly", Transport::streamable_http, {},
                         client_matrix::revisions_of_era(Transport::streamable_http, Era::legacy)});
    found.push_back(Cell{"StreamableHttp_AllRevisions_ServerPerRequestOnly", Transport::streamable_http, {},
                         client_matrix::revisions_of_era(Transport::streamable_http, Era::modern)});
    found.push_back(Cell{"StreamableHttp_AllRevisions_ServerOfBothEras", Transport::streamable_http, {}, {}});
    for (const std::string& revision : client_matrix::revisions_defining(Transport::http_sse)) {
        found.push_back(Cell{"HttpSse_Revision_" + client_matrix::test_name(revision), Transport::http_sse,
                             {revision}, {}});
    }
    found.push_back(Cell{"HttpSse_AllRevisions", Transport::http_sse, {}, {}});
    return found;
}

// The matrix server over one of the two HTTP transports, in this process, for as long as the object lives.
class OwnServer {
public:
    OwnServer(Transport transport, const std::vector<std::string>& revisions)
        : server_(client_matrix::server_options(client_matrix::profiles_of(revisions))) {
        client_matrix::register_matrix(server_);
        if (transport == Transport::http_sse) {
            http_sse_test::HttpSseServerOptions options;
            options.path = "/sse";
            sse_ = std::make_unique<http_sse_test::HttpSseServer>(server_, std::move(options));
            url_ = sse_->url();
            return;
        }
        neomcp::HttpServerOptions options;
        options.diagnostics = false;
        options.max_worker_threads = 32;
        http_ = std::make_unique<neomcp::HttpServer>(server_, std::move(options));
        const auto started = http_->start();
        if (!started) throw std::runtime_error("the server did not start: " + started.error);
        url_ = "http://127.0.0.1:" + std::to_string(*started.value) + "/mcp";
    }

    const std::string& url() const noexcept { return url_; }

private:
    neomcp::Server server_;
    std::unique_ptr<neomcp::HttpServer> http_;
    std::unique_ptr<http_sse_test::HttpSseServer> sse_;
    std::string url_;
};

// A failing cell is named by its name, not by its bytes.
void PrintTo(const Cell& cell, std::ostream* out) { *out << cell.name; }

class ClientMatrixHttp : public ::testing::TestWithParam<Cell> {};

TEST_P(ClientMatrixHttp, Flow) {
    const Cell& cell = GetParam();
    const Expected expected = client_matrix::expected_contact(cell.transport, cell.client, cell.server);
    ASSERT_FALSE(expected.revision.empty()) << "the client and the server share no revision";
    OwnServer own(cell.transport, cell.server);
    neomcp::HttpEndpoint endpoint;
    endpoint.url = own.url();
    // Loopback requests must not go to a proxy that the environment names.
    endpoint.proxy.mode = neomcp::ProxyMode::none;
    auto client = neomcp::Client::open(client_matrix::client_options(cell.client), std::move(endpoint));
    ASSERT_TRUE(client) << client.error().message;
    client_matrix::run_flow(*client, expected);
}

INSTANTIATE_TEST_SUITE_P(ClientMatrix, ClientMatrixHttp, ::testing::ValuesIn(cells()),
                         [](const ::testing::TestParamInfo<Cell>& info) { return info.param.name; });

}  // namespace
