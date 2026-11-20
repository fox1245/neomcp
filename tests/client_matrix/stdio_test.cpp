// The client matrix over stdio: the Client starts the matrix server (server.cpp) as a child process and runs the flow
// of flow.h over its pipes, once for every revision that defines stdio, and once with every revision enabled against
// servers that serve only the revisions with a handshake, only the ones that name their revision per request, and
// both.
#include <gtest/gtest.h>

#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "flow.h"
#include "neomcp/client.h"
#include "neomcp/client/stdio.h"

namespace {

using client_matrix::Expected;
using neomcp::profile::Era;
using neomcp::profile::Transport;

struct Cell {
    std::string name;
    std::vector<std::string> client;  // the revisions the client may speak; empty: all
    std::vector<std::string> server;  // the revisions the server serves; empty: all
};

std::vector<Cell> cells() {
    std::vector<Cell> found;
    for (const std::string& revision : client_matrix::revisions_defining(Transport::stdio)) {
        found.push_back(Cell{"Revision_" + client_matrix::test_name(revision), {revision}, {}});
    }
    found.push_back(Cell{"AllRevisions_ServerWithAHandshakeOnly", {},
                         client_matrix::revisions_of_era(Transport::stdio, Era::legacy)});
    found.push_back(Cell{"AllRevisions_ServerPerRequestOnly", {},
                         client_matrix::revisions_of_era(Transport::stdio, Era::modern)});
    found.push_back(Cell{"AllRevisions_ServerOfBothEras", {}, {}});
    return found;
}

// A failing cell is named by its name, not by its bytes.
void PrintTo(const Cell& cell, std::ostream* out) { *out << cell.name; }

class ClientMatrixStdio : public ::testing::TestWithParam<Cell> {};

TEST_P(ClientMatrixStdio, Flow) {
    const Cell& cell = GetParam();
    const Expected expected = client_matrix::expected_contact(Transport::stdio, cell.client, cell.server);
    ASSERT_FALSE(expected.revision.empty()) << "the client and the server share no revision";
    neomcp::ChildProcess server;
    server.program = NEOMCP_MATRIX_SERVER;
    server.arguments.emplace_back("stdio");
    for (const std::string& revision : cell.server) server.arguments.push_back(revision);
    auto client = neomcp::Client::open(client_matrix::client_options(cell.client), std::move(server));
    ASSERT_TRUE(client) << client.error().message;
    client_matrix::run_flow(*client, expected);
}

INSTANTIATE_TEST_SUITE_P(ClientMatrix, ClientMatrixStdio, ::testing::ValuesIn(cells()),
                         [](const ::testing::TestParamInfo<Cell>& info) { return info.param.name; });

}  // namespace
