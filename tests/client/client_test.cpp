// The Client against the library's own Server in the same process (neomcp/client/in_process.h): every revision over
// stdio and Streamable HTTP, input handlers of both forms, timeouts, cancellation, close and restarts.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>

#include "neomcp/client.h"
#include "neomcp/client/in_process.h"
#include "neomcp/server.h"
#include "support.h"

namespace client_pair {

struct AddArgs {
    int a;
    int b;
};

struct Folder {
    std::string path;
};

struct Confirm {
    bool ok;
};

}  // namespace client_pair

NEOMCP_DESCRIBE(client_pair::AddArgs, a, "first addend", b, "second addend")
NEOMCP_DESCRIBE(client_pair::Folder, path, "the folder")
NEOMCP_DESCRIBE(client_pair::Confirm, ok, "Delete it?")

namespace {

using namespace client_test;
using neomcp::ClientErrorKind;
using neomcp::profile::Transport;

// What the slow tool shows the test.
struct Slow {
    std::atomic<bool> started{false};
    std::atomic<bool> saw_cancel{false};
};

void populate(neomcp::Server& server, const std::shared_ptr<Slow>& slow) {
    server.tool<client_pair::AddArgs>("add", "Add two integers",
                                      [](const client_pair::AddArgs& args) { return args.a + args.b; });
    server.tool<client_pair::Folder>(
        "delete_folder", "Delete a folder",
        [](const client_pair::Folder& folder, const neomcp::Answer<client_pair::Confirm>& confirm) {
            if (!confirm.accepted() || !confirm->ok) return std::string("kept");
            return "deleted " + folder.path;
        },
        neomcp::elicit<client_pair::Confirm>(
            "confirm", [](const client_pair::Folder& folder) { return "Delete " + folder.path + "?"; }));
    server.tool("slow", "Waits until it is cancelled", [slow](neomcp::Context& ctx) {
        slow->started = true;
        eventually([&] { return ctx.cancelled(); });
        slow->saw_cancel = ctx.cancelled();
        return std::string("finished");
    });
    server.tool("failing", "Reports a failure of its own", [] {
        neomcp::ToolResult result;
        neomcp::TextContent text;
        text.text = "it failed";
        result.content.emplace_back(text);
        result.is_error = true;
        return result;
    });
}

neomcp::ServerOptions served(std::vector<std::string_view> revisions) {
    neomcp::ServerOptions options;
    options.name = "client-server";
    options.version = "1.0.0";
    for (const auto revision : revisions) options.profiles.push_back(neomcp::profile::builtin(revision));
    return options;
}

// A server, its in-process peer and a client of it.
class Pair {
public:
    Pair(neomcp::ClientOptions options, Transport transport, std::vector<std::string_view> server_revisions = {})
        : slow(std::make_shared<Slow>()), server_(served(std::move(server_revisions))) {
        populate(server_, slow);
        peer = neomcp::InProcessServer::create(server_, transport);
        auto opened = neomcp::Client::open(std::move(options), peer->transport());
        if (!opened) throw std::runtime_error("the client did not open: " + opened.error().message);
        client.emplace(std::move(opened).value());
    }

    std::shared_ptr<Slow> slow;

private:
    neomcp::Server server_;

public:
    std::shared_ptr<neomcp::InProcessServer> peer;
    std::optional<neomcp::Client> client;
};

std::string first_text(const neomcp::ToolResult& result) {
    if (result.content.empty()) return "(empty)";
    const auto* text = std::get_if<neomcp::TextContent>(&result.content[0]);
    return text == nullptr ? "(not text)" : text->text;
}

json add_arguments() { return json{{"a", 1}, {"b", 2}}; }

struct Cell {
    std::string revision;
    Transport transport;
};

std::string cell_name(const testing::TestParamInfo<Cell>& info) {
    std::string name = info.param.revision + (info.param.transport == Transport::stdio ? "_stdio" : "_http");
    for (char& c : name) {
        if (c == '-') c = '_';
    }
    return name;
}

std::vector<Cell> cells() {
    std::vector<Cell> all;
    for (const auto revision : neomcp::profile::builtin_revisions()) {
        const auto profile = neomcp::profile::builtin(revision);
        if (profile->stdio()) all.push_back(Cell{std::string(revision), Transport::stdio});
        if (profile->streamable_http()) all.push_back(Cell{std::string(revision), Transport::streamable_http});
    }
    return all;
}

bool server_asks(const neomcp::profile::Profile& profile, std::string_view method) {
    const auto* sent = profile.request(method);
    return profile.input_request(method) != nullptr || (sent != nullptr && sent->from_server);
}

neomcp::ElicitationAnswer confirmed() {
    return neomcp::ElicitationAnswer{neomcp::ElicitationAction::accept, json{{"ok", true}}};
}

class ClientPair : public testing::TestWithParam<Cell> {};

TEST_P(ClientPair, OpensRequestsPingsCallsAndCloses) {
    const Cell cell = GetParam();
    Pair pair(only({cell.revision}), cell.transport);
    neomcp::Client& client = *pair.client;

    const auto verdict = client.verdict();
    ASSERT_TRUE(verdict);
    EXPECT_EQ(verdict->revision, cell.revision);
    const neomcp::ServerDescription server = client.server();
    EXPECT_EQ(server.revision, cell.revision);
    ASSERT_TRUE(server.info);
    EXPECT_EQ(server.info->name, "client-server");

    const auto listed = client.request("tools/list");
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_TRUE(listed->contains("tools"));
    EXPECT_EQ((*listed)["tools"].size(), 4u);

    // A revision pings only when its wire tables have the request.
    const auto pong = client.ping();
    if (neomcp::profile::builtin(cell.revision)->request("ping") != nullptr) {
        EXPECT_TRUE(pong) << pong.error().message;
    } else {
        ASSERT_FALSE(pong);
        EXPECT_EQ(pong.error().kind, ClientErrorKind::unsupported_method);
    }

    const auto sum = client.call_tool("add", add_arguments());
    ASSERT_TRUE(sum) << sum.error().message;
    EXPECT_FALSE(sum->is_error);
    EXPECT_EQ(first_text(*sum), "3");

    const auto failing = client.call_tool("failing", json::object());
    ASSERT_TRUE(failing) << failing.error().message;
    EXPECT_TRUE(failing->is_error);
    EXPECT_EQ(first_text(*failing), "it failed");

    const auto missing = client.request("prompts/get", json{{"name", "no-such-prompt"}});
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().kind, ClientErrorKind::server_error);
    EXPECT_TRUE(missing.error().code.has_value());
    EXPECT_FALSE(missing.error().may_have_run);

    const auto unknown = client.request("no/such-method");
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().kind, ClientErrorKind::unsupported_method);

    EXPECT_TRUE(client.close());
    const auto after = client.ping();
    ASSERT_FALSE(after);
    EXPECT_EQ(after.error().kind, ClientErrorKind::closed);
}

TEST_P(ClientPair, AToolThatAsksIsAnsweredByAPlainHandler) {
    const Cell cell = GetParam();
    if (!server_asks(*neomcp::profile::builtin(cell.revision), "elicitation/create")) {
        GTEST_SKIP() << "the revision cannot ask for a form";
    }
    std::atomic<int> asked{0};
    auto options = only({cell.revision});
    options.handlers.form([&](const neomcp::ElicitationForm&) {
        ++asked;
        return confirmed();
    });
    Pair pair(std::move(options), cell.transport);
    const auto result = pair.client->call_tool("delete_folder", json{{"path", "/tmp/x"}});
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(first_text(*result), "deleted /tmp/x");
    EXPECT_EQ(asked.load(), 1);
}

TEST_P(ClientPair, AToolThatAsksIsAnsweredByACoroutineHandler) {
    const Cell cell = GetParam();
    if (!server_asks(*neomcp::profile::builtin(cell.revision), "elicitation/create")) {
        GTEST_SKIP() << "the revision cannot ask for a form";
    }
    std::atomic<int> asked{0};
    auto options = only({cell.revision});
    options.handlers.form([&](neomcp::ElicitationForm) -> asio::awaitable<neomcp::ElicitationAnswer> {
        ++asked;
        co_return confirmed();
    });
    Pair pair(std::move(options), cell.transport);
    const auto result = pair.client->call_tool("delete_folder", json{{"path", "/tmp/y"}});
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(first_text(*result), "deleted /tmp/y");
    EXPECT_EQ(asked.load(), 1);
}

INSTANTIATE_TEST_SUITE_P(ClientDriverEveryRevision, ClientPair, testing::ValuesIn(cells()), cell_name);

// ---------------------------------------------------------------------------------------------------------------
// Opening

TEST(Client, OpensEitherKindOfServerWithEveryProfileEnabled) {
    {
        Pair pair(only({}), Transport::stdio, {"2025-11-25"});
        const auto verdict = pair.client->verdict();
        ASSERT_TRUE(verdict);
        EXPECT_EQ(verdict->kind, neomcp::ServerKind::handshake);
        EXPECT_EQ(verdict->revision, "2025-11-25");
        EXPECT_TRUE(pair.client->ping());
    }
    {
        Pair pair(only({}), Transport::stdio, {"2026-07-28"});
        const auto verdict = pair.client->verdict();
        ASSERT_TRUE(verdict);
        EXPECT_EQ(verdict->kind, neomcp::ServerKind::per_request);
        EXPECT_EQ(verdict->revision, "2026-07-28");
        ASSERT_TRUE(pair.client->server().revisions);
        EXPECT_TRUE(pair.client->request("tools/list"));
    }
}

TEST(Client, AVerdictGivenBackOpensALaterClient) {
    std::optional<neomcp::ContactVerdict> verdict;
    {
        Pair pair(only({}), Transport::stdio, {"2025-11-25"});
        verdict = pair.client->verdict();
    }
    ASSERT_TRUE(verdict);
    auto options = only({});
    options.prior = verdict;
    Pair later(std::move(options), Transport::stdio, {"2025-11-25"});
    EXPECT_EQ(later.client->verdict(), verdict);
    EXPECT_TRUE(later.client->call_tool("add", add_arguments()));
}

TEST(Client, OptionsThatCannotWorkAreRefusedWhenOpening) {
    neomcp::Server server(served({}));
    auto peer = neomcp::InProcessServer::create(server);
    auto options = only({});
    options.max_concurrent_input_handlers = 0;
    const auto opened = neomcp::Client::open(std::move(options), peer->transport());
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().kind, ClientErrorKind::invalid_options);
    EXPECT_NE(opened.error().message.find("max_concurrent_input_handlers"), std::string::npos);
}

// Each bound the client repeats from the engine's options reaches the engine: zero, which the engine refuses, makes
// opening fail with the bound's name.
struct Field {
    std::string name;
    std::function<void(neomcp::ClientOptions&)> zero;
};

class ClientOptionField : public testing::TestWithParam<Field> {};

TEST_P(ClientOptionField, ReachesTheEngine) {
    neomcp::Server server(served({}));
    auto peer = neomcp::InProcessServer::create(server);
    auto options = only({});
    GetParam().zero(options);
    const auto opened = neomcp::Client::open(std::move(options), peer->transport());
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().kind, ClientErrorKind::invalid_options);
    EXPECT_NE(opened.error().message.find(GetParam().name), std::string::npos) << opened.error().message;
}

std::vector<Field> fields() {
    using O = neomcp::ClientOptions;
    using ms = std::chrono::milliseconds;
    return {
        {"request_timeout", [](O& o) { o.request_timeout = ms{0}; }},
        {"max_total_timeout", [](O& o) { o.max_total_timeout = ms{0}; }},
        {"discover_probe_timeout", [](O& o) { o.discover_probe_timeout = ms{0}; }},
        {"input_handler_timeout", [](O& o) { o.input_handler_timeout = ms{0}; }},
        {"max_pending_requests", [](O& o) { o.max_pending_requests = 0; }},
        {"max_queued_events", [](O& o) { o.max_queued_events = 0; }},
        {"max_message_bytes", [](O& o) { o.max_message_bytes = 0; }},
        {"max_nesting_depth", [](O& o) { o.max_nesting_depth = 0; }},
        {"max_input_rounds", [](O& o) { o.max_input_rounds = 0; }},
        {"max_input_requests_per_round", [](O& o) { o.max_input_requests_per_round = 0; }},
        {"max_concurrent_server_requests", [](O& o) { o.max_concurrent_server_requests = 0; }},
        {"peer_schema_depth", [](O& o) { o.peer_schema_depth = 0; }},
        {"peer_pattern_length", [](O& o) { o.peer_pattern_length = 0; }},
        {"peer_pattern_memory", [](O& o) { o.peer_pattern_memory = 0; }},
        {"peer_max_evaluations", [](O& o) { o.peer_max_evaluations = 0; }},
    };
}

INSTANTIATE_TEST_SUITE_P(ClientDriverOptions, ClientOptionField, testing::ValuesIn(fields()),
                         [](const testing::TestParamInfo<Field>& info) { return info.param.name; });

TEST(Client, AMovedFromClientAnswersClosed) {
    Pair pair(only({"2025-11-25"}), Transport::stdio);
    neomcp::Client moved = std::move(*pair.client);
    const auto pong = pair.client->ping();
    ASSERT_FALSE(pong);
    EXPECT_EQ(pong.error().kind, ClientErrorKind::closed);
    EXPECT_FALSE(pair.client->verdict());
    EXPECT_TRUE(moved.ping());
}

// ---------------------------------------------------------------------------------------------------------------
// Timeouts, cancellation, close

TEST(Client, ACallWhoseClockLapsesEndsTimedOutAndMayHaveRun) {
    auto timers = std::make_shared<ManualTimers>();
    auto options = only({"2025-11-25"});
    options.timers = timers;
    Pair pair(std::move(options), Transport::stdio);
    neomcp::RequestOptions call;
    call.timeout = std::chrono::milliseconds(30'000);
    std::optional<neomcp::Result<neomcp::ToolResult>> result;
    std::thread caller([&] { result.emplace(pair.client->call_tool("slow", json::object(), call)); });
    const OnExit join_caller([&] { join(caller); });
    const OnExit close_first([&] { static_cast<void>(pair.client->close()); });
    ASSERT_TRUE(eventually([&] { return pair.slow->started.load(); }));
    const auto timer = timers->live([](const ManualTimers::Started& one) {
        const auto* deadline = std::get_if<neomcp::Deadline>(&one.purpose);
        return deadline != nullptr && deadline->kind == neomcp::DeadlineKind::wait &&
               one.after == std::chrono::milliseconds(30'000);
    });
    ASSERT_TRUE(timer);
    EXPECT_TRUE(timers->fire(timer->id));
    caller.join();
    ASSERT_TRUE(result);
    ASSERT_FALSE(*result);
    EXPECT_EQ(result->error().kind, ClientErrorKind::timed_out);
    EXPECT_TRUE(result->error().may_have_run);
    EXPECT_TRUE(eventually([&] { return pair.slow->saw_cancel.load(); }));
}

TEST(Client, TheRequestTimeoutOfTheOptionsIsTheClockOfACall) {
    auto timers = std::make_shared<ManualTimers>();
    auto options = only({"2025-11-25"});
    options.timers = timers;
    options.request_timeout = std::chrono::milliseconds(12'345);
    Pair pair(std::move(options), Transport::stdio);
    EXPECT_TRUE(pair.client->ping());
    bool seen = false;
    for (const auto& one : timers->started_for<neomcp::Deadline>()) {
        seen = seen || one.after == std::chrono::milliseconds(12'345);
    }
    EXPECT_TRUE(seen);
}

TEST(Client, AStopTokenCancelsACall) {
    Pair pair(only({"2025-11-25"}), Transport::stdio);
    std::stop_source stop;
    neomcp::RequestOptions call;
    call.stop = stop.get_token();
    std::optional<neomcp::Result<neomcp::ToolResult>> result;
    std::thread caller([&] { result.emplace(pair.client->call_tool("slow", json::object(), call)); });
    const OnExit join_caller([&] { join(caller); });
    const OnExit close_first([&] { static_cast<void>(pair.client->close()); });
    ASSERT_TRUE(eventually([&] { return pair.slow->started.load(); }));
    stop.request_stop();
    caller.join();
    ASSERT_TRUE(result);
    ASSERT_FALSE(*result);
    EXPECT_EQ(result->error().kind, ClientErrorKind::cancelled);
    EXPECT_TRUE(result->error().may_have_run);
    EXPECT_TRUE(eventually([&] { return pair.slow->saw_cancel.load(); }));
}

TEST(Client, AStopRequestedBeforeTheCallSendsNothing) {
    Pair pair(only({"2025-11-25"}), Transport::stdio);
    std::stop_source stop;
    stop.request_stop();
    neomcp::RequestOptions call;
    call.stop = stop.get_token();
    const auto result = pair.client->call_tool("slow", json::object(), call);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ClientErrorKind::cancelled);
    EXPECT_FALSE(result.error().may_have_run);
    EXPECT_FALSE(pair.slow->started.load());
}

TEST(Client, CancellingTheAwaitingCoroutineCancelsTheCall) {
    using namespace asio::experimental::awaitable_operators;
    Pair pair(only({"2025-11-25"}), Transport::stdio);
    neomcp::Client& client = *pair.client;
    asio::io_context other;
    asio::steady_timer trigger(other, asio::steady_timer::time_point::max());
    std::atomic<int> winner{-1};
    asio::co_spawn(
        other,
        [&]() -> asio::awaitable<void> {
            json arguments = json::object();
            auto call = client.async_call_tool("slow", std::move(arguments));
            auto raced = co_await (std::move(call) || trigger.async_wait(asio::as_tuple(asio::use_awaitable)));
            winner = static_cast<int>(raced.index());
        },
        asio::detached);
    std::thread runner([&] { other.run(); });
    const OnExit stop_runner([&] {
        other.stop();
        join(runner);
    });
    ASSERT_TRUE(eventually([&] { return pair.slow->started.load(); }));
    asio::post(other, [&] { trigger.cancel(); });
    runner.join();
    EXPECT_EQ(winner.load(), 1);
    EXPECT_TRUE(eventually([&] { return pair.slow->saw_cancel.load(); }));
}

TEST(Client, CloseEndsThePendingCallsClosed) {
    Pair pair(only({"2025-11-25"}), Transport::stdio);
    std::optional<neomcp::Result<neomcp::ToolResult>> result;
    std::thread caller([&] { result.emplace(pair.client->call_tool("slow", json::object())); });
    const OnExit join_caller([&] { join(caller); });
    const OnExit close_first([&] { static_cast<void>(pair.client->close()); });
    ASSERT_TRUE(eventually([&] { return pair.slow->started.load(); }));
    EXPECT_TRUE(pair.client->close());
    caller.join();
    ASSERT_TRUE(result);
    ASSERT_FALSE(*result);
    EXPECT_EQ(result->error().kind, ClientErrorKind::closed);
    EXPECT_TRUE(pair.client->close());
}

// ---------------------------------------------------------------------------------------------------------------
// Restarts

TEST(Client, AServerThatExitsIsStartedAgainAtTheNextRequest) {
    for (const std::string_view revision : {std::string_view("2025-11-25"), std::string_view("2026-07-28")}) {
        SCOPED_TRACE(std::string(revision));
        std::vector<neomcp::ServerRestarted> restarts;
        std::mutex mutex;
        auto options = only({revision});
        options.on_diagnostic = [&](const neomcp::ClientDiagnostic& diagnostic) {
            if (const auto* restarted = std::get_if<neomcp::ServerRestarted>(&diagnostic)) {
                std::lock_guard lock(mutex);
                restarts.push_back(*restarted);
            }
        };
        Pair pair(std::move(options), Transport::stdio);
        EXPECT_EQ(pair.peer->processes(), 1u);
        pair.peer->end_process(1);
        const auto sum = pair.client->call_tool("add", add_arguments());
        ASSERT_TRUE(sum) << sum.error().message;
        EXPECT_EQ(first_text(*sum), "3");
        EXPECT_EQ(pair.peer->processes(), 2u);
        std::lock_guard lock(mutex);
        ASSERT_EQ(restarts.size(), 1u);
        EXPECT_EQ(restarts[0].restarts, 1u);
        EXPECT_EQ(restarts[0].handshake_repeated, revision == "2025-11-25");
    }
}

}  // namespace
