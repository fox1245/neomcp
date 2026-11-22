// What the servers of the client matrix serve: the tools, resources, resource template, prompts and completer of the
// conformance fixture (tests/conformance/everything.h), a tool whose result is a described struct (for the typed
// call), a tool that waits until it is cancelled, and a tool that asks the client's model in two rounds with a tool to
// use (tests/support/sampling_tool_loop.h). Every list comes in pages of one item, so that a client that
// lists follows several pages.
//
// The stdio program of the matrix (server.cpp) and the tests that serve over HTTP in process register the same.
#pragma once

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include <asio/as_tuple.hpp>
#include <asio/awaitable.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include "../conformance/everything.h"
#include "../support/sampling_tool_loop.h"
#include "neomcp/profile.h"
#include "neomcp/server.h"

namespace client_matrix {

struct AddArgs {
    int a;
    int b;
};

struct Sum {
    int sum;
};

struct WaitArgs {
    int milliseconds;
};

}  // namespace client_matrix

NEOMCP_DESCRIBE(client_matrix::AddArgs, a, "first addend", b, "second addend")
NEOMCP_DESCRIBE(client_matrix::Sum, sum, "the sum of the addends")
NEOMCP_DESCRIBE(client_matrix::WaitArgs, milliseconds, "how long to wait at most")

namespace client_matrix {

// The options of a matrix server serving `profiles` (empty: every built-in revision).
inline neomcp::ServerOptions server_options(std::vector<neomcp::profile::ProfilePtr> profiles = {}) {
    neomcp::ServerOptions options = conformance::server_options();
    options.name = "neomcp-client-matrix";
    options.profiles = std::move(profiles);
    options.tools_page_size = 1;
    options.resources_page_size = 1;
    options.prompts_page_size = 1;
    return options;
}

// The profiles of the revisions named, in the order given; a name without a built-in profile is skipped (the caller
// checks the count).
inline std::vector<neomcp::profile::ProfilePtr> profiles_of(const std::vector<std::string>& revisions) {
    std::vector<neomcp::profile::ProfilePtr> profiles;
    for (const std::string& revision : revisions) {
        if (auto profile = neomcp::profile::builtin(revision)) profiles.push_back(std::move(profile));
    }
    return profiles;
}

inline void register_matrix(neomcp::Server& server) {
    conformance::register_tools(server);
    conformance::register_input_tools(server);
    conformance::register_resources(server);
    conformance::register_prompts(server);
    conformance::register_changes(server);

    server.tool<AddArgs>("add", "Add two integers", [](const AddArgs& args) { return Sum{args.a + args.b}; });
    sampling_tool_loop::register_tool(server);

    // Reports once that it waits, then waits on a timer that the server cancels when the client cancels the call.
    server.tool<WaitArgs>("wait", "Wait for a time, or until the call is cancelled",
                          [](WaitArgs args, neomcp::Context& context) -> asio::awaitable<std::string> {
                              const auto executor = co_await asio::this_coro::executor;
                              asio::steady_timer timer(executor, std::chrono::milliseconds(args.milliseconds));
                              context.progress(0, std::nullopt, "waiting");
                              co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
                              co_return std::string("waited");
                          });
}

}  // namespace client_matrix
