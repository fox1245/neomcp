// The client against a server program given on the command line, over every transport and revision: the flow of
// flow.h against a peer that serves what registrations.h registers, under the same names. It runs against the
// library's own matrix server (server.cpp), and in the interoperability job against the official Python SDK's servers
// (tests/interop/python_server.py).
//
//   neomcp_client_matrix_peer_test [GoogleTest flags] [--paged] -- <program> [argument...]
//
// The program is started with the transport as one more argument: `stdio` (the client starts it and speaks over its
// pipes), `http` (Streamable HTTP at /mcp) or `sse` (the transport of events over HTTP at /sse). Over HTTP it must
// write the port it listens on as the first line of its standard error. `--paged`: the peer lists in pages of one
// item. Each cell is one revision the transport defines, with a client that speaks only it, and one client that
// speaks every revision.
#include <gtest/gtest.h>

#include <cstdio>
#include <exception>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "flow.h"
#include "neomcp/client.h"
#include "neomcp/client/http.h"
#include "neomcp/client/stdio.h"
#include "process.h"

namespace {

using client_matrix::Expected;
using neomcp::profile::Transport;

std::vector<std::string> peer_command;
bool peer_pages = false;

struct Cell {
    std::string name;
    Transport transport = Transport::stdio;
    std::vector<std::string> client;  // the revisions the client may speak; empty: all
};

struct Served {
    Transport transport;
    const char* label;
    const char* mode;
    const char* path;
};

constexpr Served served[] = {
    {Transport::stdio, "Stdio", "stdio", ""},
    {Transport::streamable_http, "StreamableHttp", "http", "/mcp"},
    {Transport::http_sse, "HttpSse", "sse", "/sse"},
};

const Served& served_over(Transport transport) {
    for (const Served& entry : served) {
        if (entry.transport == transport) return entry;
    }
    return served[0];
}

std::vector<Cell> cells() {
    std::vector<Cell> found;
    for (const Served& entry : served) {
        for (const std::string& revision : client_matrix::revisions_defining(entry.transport)) {
            found.push_back(Cell{std::string(entry.label) + "_Revision_" + client_matrix::test_name(revision),
                                 entry.transport, {revision}});
        }
        found.push_back(Cell{std::string(entry.label) + "_AllRevisions", entry.transport, {}});
    }
    return found;
}

// A failing cell is named by its name, not by its bytes.
void PrintTo(const Cell& cell, std::ostream* out) { *out << cell.name; }

class ClientMatrixPeer : public ::testing::TestWithParam<Cell> {};

TEST_P(ClientMatrixPeer, Flow) {
    const Cell& cell = GetParam();
    ASSERT_FALSE(peer_command.empty());
    Expected expected = client_matrix::expected_contact(cell.transport, cell.client, {});
    ASSERT_FALSE(expected.revision.empty());
    expected.paged = peer_pages;
    const Served& entry = served_over(cell.transport);
    std::vector<std::string> command = peer_command;
    command.emplace_back(entry.mode);

    if (cell.transport == Transport::stdio) {
        neomcp::ChildProcess process;
        process.program = command.front();
        process.arguments.assign(command.begin() + 1, command.end());
        auto client = neomcp::Client::open(client_matrix::client_options(cell.client), std::move(process));
        ASSERT_TRUE(client) << client.error().message;
        client_matrix::run_flow(*client, expected);
        return;
    }

    std::unique_ptr<client_matrix::ServerProcess> server;
    try {
        server = std::make_unique<client_matrix::ServerProcess>(command);
    } catch (const std::exception& failure) {
        FAIL() << failure.what();
    }
    const std::string url = "http://127.0.0.1:" + std::to_string(server->port()) + entry.path;
    neomcp::HttpEndpoint endpoint;
    endpoint.url = url;
    // Loopback requests must not go to a proxy that the environment names.
    endpoint.proxy.mode = neomcp::ProxyMode::none;
    auto client = neomcp::Client::open(client_matrix::client_options(cell.client), std::move(endpoint));
    ASSERT_TRUE(client) << client.error().message << " (" << url << ")";
    client_matrix::run_flow(*client, expected);
}

INSTANTIATE_TEST_SUITE_P(ClientMatrix, ClientMatrixPeer, ::testing::ValuesIn(cells()),
                         [](const ::testing::TestParamInfo<Cell>& info) { return info.param.name; });

int usage() {
    std::fputs("usage: neomcp_client_matrix_peer_test [GoogleTest flags] [--paged] -- <program> [argument...]\n",
               stderr);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    int at = 1;
    for (; at < argc; ++at) {
        const std::string_view argument = argv[at];
        if (argument == "--") {
            ++at;
            break;
        }
        if (argument != "--paged") return usage();
        peer_pages = true;
    }
    for (; at < argc; ++at) peer_command.emplace_back(argv[at]);
    if (peer_command.empty()) return usage();
    return RUN_ALL_TESTS();
}
