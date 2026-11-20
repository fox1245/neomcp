// The Client over the child-process transport: against the library's own stdio server on every revision, the probe
// against servers of one kind, a program that cannot be found, and servers that end while calls wait.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <filesystem>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "support.h"

#ifndef _WIN32
#include <signal.h>
#include <sys/types.h>
#endif

using namespace client_stdio_test;
using neomcp::ChildProcessStep;

namespace {

neomcp::ChildProcess program(const std::string& path, std::vector<std::string> arguments = {}) {
    neomcp::ChildProcess process;
    process.program = path;
    process.arguments = std::move(arguments);
    return process;
}

// The sum in a result of the fixture's `add`: in the structured content, or (where the revision has none) in the
// text block that holds the same object.
std::optional<int> sum_of(const neomcp::ToolResult& result) {
    if (result.structured_content && result.structured_content->contains("sum")) {
        return (*result.structured_content)["sum"].get<int>();
    }
    for (const auto& block : result.content) {
        if (const auto* text = std::get_if<neomcp::TextContent>(&block)) {
            const json value = json::parse(text->text, nullptr, false);
            if (!value.is_discarded() && value.is_object() && value.contains("sum")) return value["sum"].get<int>();
        }
    }
    return std::nullopt;
}

std::string text_of(const neomcp::ToolResult& result) {
    for (const auto& block : result.content) {
        if (const auto* text = std::get_if<neomcp::TextContent>(&block)) return text->text;
    }
    return {};
}

neomcp::Result<neomcp::Client> open_with(neomcp::ClientOptions options, neomcp::ChildProcess process, Steps& steps) {
    auto transport = transport_of(std::move(process), steps, options.max_message_bytes);
    if (!transport) {
        neomcp::ClientError error;
        error.kind = neomcp::ClientErrorKind::invalid_options;
        error.message = "the transport could not be made";
        return error;
    }
    return neomcp::Client::open(std::move(options), std::move(transport));
}

// Opens on another thread and waits for it within the bound. An open that does not return would hold the test for
// ever: the program ends then, and with it (POSIX) the groups of the processes the transport started.
neomcp::Result<neomcp::Client> open_within_bound(neomcp::ClientOptions options, neomcp::ChildProcess process,
                                                 Steps& steps) {
    auto transport = transport_of(std::move(process), steps, options.max_message_bytes);
    auto done = std::make_shared<std::promise<neomcp::Result<neomcp::Client>>>();
    auto opened = done->get_future();
    std::thread([options = std::move(options), transport = std::move(transport), done]() mutable {
        done->set_value(neomcp::Client::open(std::move(options), std::move(transport)));
    }).detach();
    if (opened.wait_for(wait_bound) != std::future_status::ready) {
        std::fprintf(stderr, "Client::open did not return within the bound\n");
#ifndef _WIN32
        for (const auto& event : steps.all()) {
            if (event.step == ChildProcessStep::started) ::kill(-static_cast<pid_t>(event.pid), SIGKILL);
        }
#endif
        std::fflush(stderr);
        std::_Exit(1);
    }
    return opened.get();
}

}  // namespace

TEST(ClientStdio, ACallRoundTripsWithTheLibraryServerOnEveryRevision) {
    for (const std::string_view revision : neomcp::profile::builtin_revisions()) {
        SCOPED_TRACE(std::string(revision));
        auto client = neomcp::Client::open(only({revision}), program(library_server));
        ASSERT_TRUE(client) << client.error().message;
        EXPECT_EQ(client->server().revision, revision);
        json arguments = {{"a", 2}, {"b", 3}};
        auto result = client->call_tool("add", std::move(arguments));
        ASSERT_TRUE(result) << result.error().message;
        EXPECT_FALSE(result->is_error);
        EXPECT_EQ(sum_of(*result), 5);
        auto failed = client->call_tool("fail");
        ASSERT_TRUE(failed) << failed.error().message;
        EXPECT_TRUE(failed->is_error);
        EXPECT_TRUE(client->close());
    }
}

TEST(ClientStdio, AsyncOpenTakesAChildProcessToo) {
    asio::io_context io;
    auto opened = asio::co_spawn(io, neomcp::Client::async_open(every_revision(), program(library_server)),
                                 asio::use_future);
    io.run();
    auto client = opened.get();
    ASSERT_TRUE(client) << client.error().message;
    json arguments = {{"text", "over a pipe"}};
    auto result = client->call_tool("echo", std::move(arguments));
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(text_of(*result), "over a pipe");
}

TEST(ClientStdio, TheProbeFindsAServerThatOnlyHandshakes) {
    auto client = neomcp::Client::open(every_revision(), program(profile_server, {"2025-11-25"}));
    ASSERT_TRUE(client) << client.error().message;
    const auto verdict = client->verdict();
    ASSERT_TRUE(verdict.has_value());
    EXPECT_EQ(verdict->kind, neomcp::ServerKind::handshake);
    EXPECT_EQ(verdict->revision, "2025-11-25");
    json arguments = {{"a", 20}, {"b", 22}};
    auto result = client->call_tool("sum", std::move(arguments));
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(text_of(*result), "42");
}

TEST(ClientStdio, TheProbeFindsAServerThatOnlyNamesRevisionsPerRequest) {
    auto client = neomcp::Client::open(every_revision(), program(profile_server, {"2026-07-28"}));
    ASSERT_TRUE(client) << client.error().message;
    const auto verdict = client->verdict();
    ASSERT_TRUE(verdict.has_value());
    EXPECT_EQ(verdict->kind, neomcp::ServerKind::per_request);
    EXPECT_EQ(verdict->revision, "2026-07-28");
    json arguments = {{"a", 1}, {"b", 1}};
    auto result = client->call_tool("sum", std::move(arguments));
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(text_of(*result), "2");
}

TEST(ClientStdio, AServerThatEndsOnTheProbeIsStartedOnceMoreForTheHandshake) {
    Folder folder;
    json rules = json::array();
    rules.push_back(rule("server/discover", json::array({action("exit", 0)})));
    rules.push_back(rule("initialize", json::array({action("answer", initialize_result("2025-11-25"))})));
    rules.push_back(tool_rule("hello", json::array({action("answer", json{{"content", json::array()}})})));
    json script = json::object();
    script["rules"] = rules;
    Steps steps;
    auto client = open_with(every_revision(), scripted(folder, script), steps);
    ASSERT_TRUE(client) << client.error().message;
    EXPECT_EQ(client->verdict()->kind, neomcp::ServerKind::handshake);
    EXPECT_EQ(steps.count(ChildProcessStep::started), 2u);
    auto result = client->call_tool("hello");
    ASSERT_TRUE(result) << result.error().message;
}

TEST(ClientStdio, AProgramThatCannotBeFoundIsATransportError) {
    auto client = neomcp::Client::open(every_revision(), program("neomcp-no-such-program-anywhere"));
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().kind, neomcp::ClientErrorKind::transport);
}

TEST(ClientStdio, ADescriptionThatCannotWorkIsInvalidOptions) {
    auto client = neomcp::Client::open(every_revision(), program(""));
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().kind, neomcp::ClientErrorKind::invalid_options);
}

TEST(ClientStdio, BannersWithinTheBoundDoNotDisturbOpening) {
    Folder folder;
    json script = json::object();
    script["start"] = json::array({action("lines", json{{"text", "starting up"}, {"count", 3}})});
    script["rules"] = json::array({rule("initialize", json::array({action("answer", initialize_result("2025-11-25"))}))});
    std::vector<neomcp::DiagnosticKind> seen;
    std::mutex mutex;
    neomcp::ClientOptions options = only({"2025-11-25"});
    options.on_diagnostic = [&](const neomcp::ClientDiagnostic& diagnostic) {
        if (const auto* engine = std::get_if<neomcp::Diagnostic>(&diagnostic)) {
            std::lock_guard lock(mutex);
            seen.push_back(engine->kind);
        }
    };
    auto client = neomcp::Client::open(std::move(options), scripted(folder, script));
    ASSERT_TRUE(client) << client.error().message;
    std::lock_guard lock(mutex);
    EXPECT_EQ(std::count(seen.begin(), seen.end(), neomcp::DiagnosticKind::not_json), 3);
}

TEST(ClientStdio, CallsPendingWhenTheServerEndsEndServerExited) {
    Folder folder;
    const std::string held = folder.file("held");
    json rules = json::array();
    rules.push_back(rule("initialize", json::array({action("answer", initialize_result("2025-11-25"))})));
    rules.push_back(tool_rule("hold", json::array({action("append", json{{"path", held}, {"text", "held"}})})));
    rules.push_back(tool_rule("die", json::array({action("exit", 7)})));
    json script = json::object();
    script["rules"] = rules;
    Steps steps;
    neomcp::ClientOptions options = only({"2025-11-25"});
    options.max_restarts = 0;
    auto client = open_with(std::move(options), scripted(folder, script), steps);
    ASSERT_TRUE(client) << client.error().message;
    auto waiting = std::async(std::launch::async, [&] { return client->call_tool("hold"); });
    ASSERT_TRUE(eventually([&] { return std::filesystem::exists(held); }));
    auto died = client->call_tool("die");
    auto held_result = waiting.get();
    for (const auto* result : {&died, &held_result}) {
        ASSERT_FALSE(*result);
        EXPECT_EQ(result->error().kind, neomcp::ClientErrorKind::server_exited);
        EXPECT_TRUE(result->error().may_have_run);
    }
    const auto exited = steps.await(ChildProcessStep::exited);
    ASSERT_TRUE(exited.has_value());
    ASSERT_TRUE(exited->exit_code.has_value());
    EXPECT_EQ(*exited->exit_code, 7);
}

TEST(ClientStdio, TheServerIsStartedAgainUpToMaxRestartsInARow) {
    Folder folder;
    json rules = json::array();
    rules.push_back(rule("server/discover", json::array({action("answer", discover_result("2026-07-28"))})));
    rules.push_back(tool_rule("die", json::array({action("exit", 3)})));
    json script = json::object();
    script["rules"] = rules;
    Steps steps;
    neomcp::ClientOptions options = only({"2026-07-28"});
    options.max_restarts = 2;
    auto client = open_with(std::move(options), scripted(folder, script), steps);
    ASSERT_TRUE(client) << client.error().message;
    // Every call ends the process it reaches; each later call starts a new one, until the bound.
    for (int call = 0; call < 3; ++call) {
        SCOPED_TRACE(call);
        auto result = client->call_tool("die");
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().kind, neomcp::ClientErrorKind::server_exited);
    }
    auto refused = client->call_tool("die");
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().kind, neomcp::ClientErrorKind::too_many_restarts);
    EXPECT_EQ(steps.count(ChildProcessStep::started), 3u);
}

TEST(ClientStdio, AServerThatAnswersAgainAfterARestartIsUsed) {
    Folder folder;
    json rules = json::array();
    rules.push_back(rule("server/discover", json::array({action("answer", discover_result("2026-07-28"))})));
    rules.push_back(tool_rule("die", json::array({action("exit", 3)})));
    const json complete{{"content", json::array()}, {"resultType", "complete"}};
    rules.push_back(tool_rule("hello", json::array({action("answer", complete)})));
    json script = json::object();
    script["rules"] = rules;
    Steps steps;
    auto client = open_with(only({"2026-07-28"}), scripted(folder, script), steps);
    ASSERT_TRUE(client) << client.error().message;
    auto died = client->call_tool("die");
    ASSERT_FALSE(died);
    auto again = client->call_tool("hello");
    ASSERT_TRUE(again) << again.error().message;
    EXPECT_EQ(steps.count(ChildProcessStep::started), 2u);
}

TEST(ClientStdio, AnAnswerToNoPendingRequestIsDroppedAndTheCallGoesOn) {
    Folder folder;
    json rules = json::array();
    rules.push_back(rule("initialize", json::array({action("answer", initialize_result("2025-11-25"))})));
    const json stray{{"jsonrpc", "2.0"}, {"id", 987654}, {"result", json::object()}};
    rules.push_back(tool_rule("hello", json::array({action("line", stray.dump()),
                                                    action("answer", json{{"content", json::array()}})})));
    json script = json::object();
    script["rules"] = rules;
    std::vector<neomcp::DiagnosticKind> seen;
    std::mutex mutex;
    neomcp::ClientOptions options = only({"2025-11-25"});
    options.on_diagnostic = [&](const neomcp::ClientDiagnostic& diagnostic) {
        if (const auto* engine = std::get_if<neomcp::Diagnostic>(&diagnostic)) {
            std::lock_guard lock(mutex);
            seen.push_back(engine->kind);
        }
    };
    auto client = neomcp::Client::open(std::move(options), scripted(folder, script));
    ASSERT_TRUE(client) << client.error().message;
    auto result = client->call_tool("hello");
    ASSERT_TRUE(result) << result.error().message;
    std::lock_guard lock(mutex);
    EXPECT_EQ(std::count(seen.begin(), seen.end(), neomcp::DiagnosticKind::unknown_id), 1);
}

TEST(ClientStdio, AServerThatHandshakesIsStartedAgainWithANewHandshake) {
    Folder folder;
    json rules = json::array();
    rules.push_back(rule("initialize", json::array({action("answer", initialize_result("2025-11-25"))})));
    rules.push_back(tool_rule("die", json::array({action("exit", 3)})));
    rules.push_back(tool_rule("hello", json::array({action("answer", json{{"content", json::array()}})})));
    json script = json::object();
    script["rules"] = rules;
    std::vector<neomcp::ServerRestarted> restarts;
    std::mutex mutex;
    neomcp::ClientOptions options = only({"2025-11-25"});
    options.on_diagnostic = [&](const neomcp::ClientDiagnostic& diagnostic) {
        if (const auto* restarted = std::get_if<neomcp::ServerRestarted>(&diagnostic)) {
            std::lock_guard lock(mutex);
            restarts.push_back(*restarted);
        }
    };
    Steps steps;
    auto client = open_with(std::move(options), scripted(folder, script), steps);
    ASSERT_TRUE(client) << client.error().message;
    auto died = client->call_tool("die");
    ASSERT_FALSE(died);
    EXPECT_EQ(died.error().kind, neomcp::ClientErrorKind::server_exited);
    auto again = client->call_tool("hello");
    ASSERT_TRUE(again) << again.error().message;
    EXPECT_EQ(steps.count(ChildProcessStep::started), 2u);
    std::lock_guard lock(mutex);
    ASSERT_EQ(restarts.size(), 1u);
    EXPECT_TRUE(restarts.front().handshake_repeated);
}

TEST(ClientStdio, ACallPendingWhenTheServerWritesTooMuchThatIsNotAMessageEndsServerExited) {
    Folder folder;
    json rules = json::array();
    rules.push_back(rule("initialize", json::array({action("answer", initialize_result("2025-11-25"))})));
    rules.push_back(tool_rule("spew", json::array({action("lines", json{{"text", "not a message"}, {"count", 3}})})));
    json script = json::object();
    script["rules"] = rules;
    script["end"] = json::array();
    neomcp::ChildProcess process = scripted(folder, script);
    process.max_ignored_stdout_lines = 2;
    Steps steps;
    neomcp::ClientOptions options = only({"2025-11-25"});
    options.max_restarts = 0;
    auto client = open_with(std::move(options), process, steps);
    ASSERT_TRUE(client) << client.error().message;
    auto result = client->call_tool("spew");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, neomcp::ClientErrorKind::server_exited);
    EXPECT_TRUE(result.error().may_have_run);
    const auto killed = steps.await(ChildProcessStep::kill_sent);
    ASSERT_TRUE(killed.has_value());
    EXPECT_EQ(killed->cause, neomcp::ChildProcessCause::output_refused);
}

TEST(ClientStdio, AServerThatEndsDuringEveryHandshakeFailsOpeningAfterMaxRestarts) {
    Folder folder;
    json rules = json::array();
    rules.push_back(rule("initialize", json::array({action("exit", 5)})));
    json script = json::object();
    script["rules"] = rules;
    neomcp::ClientOptions options = only({"2025-11-25"});
    options.max_restarts = 2;
    Steps steps;
    auto client = open_within_bound(std::move(options), scripted(folder, script), steps);
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().kind, neomcp::ClientErrorKind::too_many_restarts);
    EXPECT_EQ(steps.count(ChildProcessStep::started), 3u);
}

TEST(ClientStdio, AServerThatEndsDuringEveryProbeFailsOpeningAfterMaxRestarts) {
    Folder folder;
    json rules = json::array();
    rules.push_back(rule("server/discover", json::array({action("exit", 5)})));
    json script = json::object();
    script["rules"] = rules;
    neomcp::ClientOptions options = only({"2026-07-28"});
    options.max_restarts = 1;
    Steps steps;
    auto client = open_within_bound(std::move(options), scripted(folder, script), steps);
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().kind, neomcp::ClientErrorKind::too_many_restarts);
    // The first start after the probe does not count; the next ones do.
    EXPECT_EQ(steps.count(ChildProcessStep::started), 3u);
}

TEST(ClientStdio, MaxRestartsHoldsUnderAHandshakeRevision) {
    Folder folder;
    json rules = json::array();
    rules.push_back(rule("initialize", json::array({action("answer", initialize_result("2025-11-25"))})));
    rules.push_back(tool_rule("die", json::array({action("exit", 3)})));
    json script = json::object();
    script["rules"] = rules;
    neomcp::ClientOptions options = only({"2025-11-25"});
    options.max_restarts = 1;
    Steps steps;
    auto client = open_with(std::move(options), scripted(folder, script), steps);
    ASSERT_TRUE(client) << client.error().message;
    // The answer to each new handshake is not an answer between the exits: the second exit in a row is past the
    // bound.
    for (int call = 0; call < 2; ++call) {
        SCOPED_TRACE(call);
        auto result = client->call_tool("die");
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().kind, neomcp::ClientErrorKind::server_exited);
    }
    auto refused = client->call_tool("die");
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().kind, neomcp::ClientErrorKind::too_many_restarts);
    EXPECT_EQ(steps.count(ChildProcessStep::started), 2u);
}
