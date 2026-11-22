// The flow every cell of the client matrix runs, against a server that serves what registrations.h registers (the
// library's own servers, and the Python SDK's servers of tests/interop/python_server.py, which serve the same names):
// the kind of server and the revision that contact found, lists, a plain and a typed tool call, resources and a
// resource template, a prompt, completion, ping, progress, the server's requests for input (a form, the client's
// model, the model in two rounds with a tool to use, its roots), cancellation, a change of the tools, and close.
//
// What a step expects comes from the governing revision's profile: a typed call where tool results carry structured
// content (else the typed call must fail at once, and an untyped one carries the result as text), completion and ping
// where the revision defines them, each kind of input request where the revision lets a server ask it, and sampling
// with a tool where the revision's sampling carries tool uses and results.
#pragma once

#include <string>
#include <vector>

#include "neomcp/client.h"
#include "neomcp/profile.h"

namespace client_matrix {

struct Expected {
    std::string revision;
    neomcp::ServerKind kind = neomcp::ServerKind::per_request;
    neomcp::profile::Transport transport = neomcp::profile::Transport::stdio;
    // The server lists in pages of one item (the library's own servers); the Python SDK's servers send one page.
    bool paged = true;
};

// The options of a client that may speak `revisions` (empty: every built-in revision), with the input handlers the
// flow's asking tools need.
neomcp::ClientOptions client_options(const std::vector<std::string>& revisions);

// What contact finds when a client that may speak `client_revisions` (empty: all) meets a server that serves
// `server_revisions` (empty: all) over `transport`: the newest revision both have that defines the transport, and the
// kind of server its era makes. Empty revision when they share none.
Expected expected_contact(neomcp::profile::Transport transport, const std::vector<std::string>& client_revisions,
                          const std::vector<std::string>& server_revisions);

// The built-in revisions that define `transport`, in ascending order.
std::vector<std::string> revisions_defining(neomcp::profile::Transport transport);
// The built-in revisions of one era that define `transport`.
std::vector<std::string> revisions_of_era(neomcp::profile::Transport transport, neomcp::profile::Era era);

// Runs the flow on an open client and closes it. Reports failures through GoogleTest, each under the name of its step.
void run_flow(neomcp::Client& client, const Expected& expected);

// A name for a test parameter: letters, digits and underscores.
std::string test_name(std::string text);

}  // namespace client_matrix
