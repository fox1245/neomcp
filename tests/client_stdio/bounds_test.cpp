// The bounds of the child-process transport, each driven to its edge by the scripted server: a line at the bound of a
// message and one byte longer, the lines and bytes of output that are not messages, standard error's lines and tail,
// and the graces before each step that ends a process (measured, since the wait itself is what they bound).
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "support.h"

using namespace client_stdio_test;
using neomcp::ChildProcessStep;
using neomcp::DiagnosticKind;

namespace {

constexpr std::string_view revision = "2025-11-25";

json empty_result() { return json{{"content", json::array()}}; }

// The engine's diagnostics of one kind, counted as they arrive.
struct Kinds {
    std::mutex mutex;
    std::vector<DiagnosticKind> seen;

    void listen(neomcp::ClientOptions& options) {
        options.on_diagnostic = [this](const neomcp::ClientDiagnostic& diagnostic) {
            if (const auto* engine = std::get_if<neomcp::Diagnostic>(&diagnostic)) {
                std::lock_guard lock(mutex);
                seen.push_back(engine->kind);
            }
        };
    }
    std::size_t count(DiagnosticKind kind) {
        std::lock_guard lock(mutex);
        return static_cast<std::size_t>(std::count(seen.begin(), seen.end(), kind));
    }
};

// A script whose server opens with a handshake and answers each tool by `tools` (name, actions).
json script_of(const std::vector<std::pair<std::string, json>>& tools) {
    json rules = json::array();
    rules.push_back(rule("initialize", json::array({action("answer", initialize_result(revision))})));
    for (const auto& [name, actions] : tools) rules.push_back(tool_rule(name, actions));
    json script = json::object();
    script["rules"] = rules;
    return script;
}

json sized(std::size_t bytes, std::string_view end) {
    return action("sized_answer", json{{"result", empty_result()}, {"bytes", bytes}, {"end", std::string(end)}});
}

}  // namespace

// ---- One message

TEST(ClientBounds, ALineAtTheMessageBoundIsReadAndOneByteLongerIsSkippedAndCounted) {
    constexpr std::size_t bound = std::size_t{64} * 1024;
    Folder folder;
    const json answer = action("answer", empty_result());
    const json script = script_of({{"at", json::array({sized(bound, "\n")})},
                                   {"at_crlf", json::array({sized(bound, "\r\n")})},
                                   {"past", json::array({sized(bound + 1, "\n"), answer})},
                                   {"past_crlf", json::array({sized(bound + 1, "\r\n"), answer})}});
    Kinds kinds;
    auto options = only({revision});
    options.max_message_bytes = bound;
    kinds.listen(options);
    auto client = neomcp::Client::open(std::move(options), scripted(folder, script));
    ASSERT_TRUE(client) << client.error().message;
    for (const char* name : {"at", "at_crlf"}) {
        auto result = client->call_tool(name);
        ASSERT_TRUE(result) << name << ": " << result.error().message;
    }
    EXPECT_EQ(kinds.count(DiagnosticKind::too_large), 0u);
    // The line past the bound is skipped (the call goes on and takes the answer after it).
    std::size_t skipped = 0;
    for (const char* name : {"past", "past_crlf"}) {
        auto result = client->call_tool(name);
        ASSERT_TRUE(result) << name << ": " << result.error().message;
        EXPECT_EQ(kinds.count(DiagnosticKind::too_large), ++skipped) << name;
    }
    EXPECT_EQ(kinds.count(DiagnosticKind::not_json), 0u);
}

// A transport made apart from the client (make_transport) keeps the bound it was made with: a line it cuts there is a
// line too long for the client too, whatever bound the client's own options name.
TEST(ClientBounds, ALineOverTheTransportsBoundIsTooLongForTheClient) {
    constexpr std::size_t bound = 1024;
    Folder folder;
    const json script = script_of({{"past", json::array({sized(2 * bound, "\n"), action("answer", empty_result())})}});
    Kinds kinds;
    auto options = only({revision});
    kinds.listen(options);
    Steps steps;
    auto transport = transport_of(scripted(folder, script), steps, bound);
    ASSERT_TRUE(transport);
    auto client = neomcp::Client::open(std::move(options), std::move(transport));
    ASSERT_TRUE(client) << client.error().message;
    auto result = client->call_tool("past");
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(kinds.count(DiagnosticKind::too_large), 1u);
    EXPECT_EQ(kinds.count(DiagnosticKind::not_json), 0u);
}

// ---- Output that is not a message

TEST(ClientBounds, LinesThatAreNotMessagesUpToTheBoundAreSkippedAndOneMoreEndsTheServer) {
    constexpr std::size_t bound = neomcp::ChildProcessDefaults::max_ignored_stdout_lines;
    Folder folder;
    const json answer = action("answer", empty_result());
    const json script = script_of(
        {{"at", json::array({action("lines", json{{"text", "a banner"}, {"count", bound}}), answer})},
         {"one_more", json::array({action("line", "one more banner"), answer})},
         {"after", json::array({answer})}});
    Steps steps;
    auto transport = transport_of(scripted(folder, script), steps);
    ASSERT_TRUE(transport);
    auto client = neomcp::Client::open(only({revision}), std::move(transport));
    ASSERT_TRUE(client) << client.error().message;
    auto at = client->call_tool("at");
    ASSERT_TRUE(at) << at.error().message;
    EXPECT_EQ(steps.count(ChildProcessStep::kill_sent), 0u);
    auto past = client->call_tool("one_more");
    ASSERT_FALSE(past);
    EXPECT_EQ(past.error().kind, neomcp::ClientErrorKind::server_exited);
    const auto killed = steps.await(ChildProcessStep::kill_sent);
    ASSERT_TRUE(killed.has_value());
    EXPECT_EQ(killed->cause, neomcp::ChildProcessCause::output_refused);
    // The next call starts the server again.
    auto after = client->call_tool("after");
    ASSERT_TRUE(after) << after.error().message;
    EXPECT_EQ(steps.count(ChildProcessStep::started), 2u);
}

TEST(ClientBounds, BytesThatAreNotMessagesUpToTheBoundAreSkippedAndOneMoreEndsTheServer) {
    constexpr std::size_t bound = neomcp::ChildProcessDefaults::max_ignored_stdout_bytes;
    constexpr std::size_t line = 1024;
    static_assert(bound % line == 0);
    Folder folder;
    const json answer = action("answer", empty_result());
    const json script = script_of(
        {{"at", json::array({action("lines", json{{"text", std::string(line, 'b')}, {"count", bound / line}}),
                             answer})},
         {"one_more", json::array({action("line", "b"), answer})}});
    Steps steps;
    auto transport = transport_of(scripted(folder, script), steps);
    ASSERT_TRUE(transport);
    auto options = only({revision});
    options.max_restarts = 0;
    auto client = neomcp::Client::open(std::move(options), std::move(transport));
    ASSERT_TRUE(client) << client.error().message;
    auto at = client->call_tool("at");
    ASSERT_TRUE(at) << at.error().message;
    EXPECT_EQ(steps.count(ChildProcessStep::kill_sent), 0u);
    auto past = client->call_tool("one_more");
    ASSERT_FALSE(past);
    EXPECT_EQ(past.error().kind, neomcp::ClientErrorKind::server_exited);
    const auto killed = steps.await(ChildProcessStep::kill_sent);
    ASSERT_TRUE(killed.has_value());
    EXPECT_EQ(killed->cause, neomcp::ChildProcessCause::output_refused);
}

// ---- Standard error

TEST(ClientBounds, StandardErrorKeepsLinesUpToTheirBoundAndTheTailUpToItsBound) {
    constexpr std::size_t line_bound = neomcp::ChildProcessDefaults::max_stderr_line_bytes;
    constexpr std::size_t tail_bound = neomcp::ChildProcessDefaults::stderr_tail_bytes;
    const std::string at(line_bound, 'a');
    const std::string past = std::string(line_bound, 'b') + "c";
    std::string rest;
    for (std::size_t i = 0; rest.size() <= tail_bound; ++i) rest += std::to_string(i % 10);
    const std::string written = at + "\n" + past + "\n" + rest;
    Folder folder;
    neomcp::ChildProcess process =
        scripted(folder, json{{"start", json::array({action("stderr", written), action("exit", 0)})}});
    process.stderr_mode = neomcp::StderrMode::capture;
    std::mutex mutex;
    std::vector<std::string> lines;
    process.on_stderr_line = [&](std::string_view line) {
        std::lock_guard lock(mutex);
        lines.emplace_back(line);
    };
    Rig rig;
    Steps steps;
    auto transport = transport_of(process, steps);
    ASSERT_TRUE(transport);
    ASSERT_FALSE(rig.start(*transport).has_value());
    const auto exited = steps.await(ChildProcessStep::exited);
    ASSERT_TRUE(rig.close(*transport));
    ASSERT_TRUE(exited.has_value());
    EXPECT_EQ(exited->stderr_tail, written.substr(written.size() - tail_bound));
    std::lock_guard lock(mutex);
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0], at);
    EXPECT_EQ(lines[1], past.substr(0, line_bound));
    EXPECT_EQ(lines[2], rest.substr(0, line_bound));
}

// ---- The graces before each step that ends a server

#ifndef _WIN32
TEST(ClientBounds, TheGracesAreWaitedInFullBeforeEachStep) {
    using Clock = std::chrono::steady_clock;
    Folder folder;
    neomcp::ChildProcess process = scripted(
        folder, json{{"start", json::array({action("ignore_sigterm", true), action("line", R"({"ready":true})")})},
                     {"end", json::array()}});
    std::mutex mutex;
    std::vector<std::pair<ChildProcessStep, Clock::time_point>> steps;
    neomcp::ChildProcessTransportOptions options;
    options.observer = [&](const neomcp::ChildProcessEvent& event) {
        std::lock_guard lock(mutex);
        steps.emplace_back(event.step, Clock::now());
    };
    // The rig holds the context the transport's timers run on: it is made first, so that it ends after the transport.
    Rig rig;
    auto made = neomcp::make_transport(process, std::move(options));
    ASSERT_TRUE(made);
    auto transport = std::move(*made);
    ASSERT_FALSE(rig.start(*transport).has_value());
    ASSERT_TRUE(eventually([&] { return rig.lines().size() == 1; }));
    ASSERT_TRUE(rig.close(*transport));
    const auto at = [&](ChildProcessStep step) -> std::optional<Clock::time_point> {
        std::lock_guard lock(mutex);
        for (const auto& [one, when] : steps) {
            if (one == step) return when;
        }
        return std::nullopt;
    };
    const auto closed = at(ChildProcessStep::input_closed);
    const auto terminated = at(ChildProcessStep::terminate_sent);
    const auto killed = at(ChildProcessStep::kill_sent);
    ASSERT_TRUE(closed && terminated && killed);
    EXPECT_GE(*terminated - *closed, neomcp::ChildProcessDefaults::exit_grace);
    EXPECT_GE(*killed - *terminated, neomcp::ChildProcessDefaults::term_grace);
    using std::chrono::duration_cast;
    using std::chrono::milliseconds;
    const auto exit_waited = duration_cast<milliseconds>(*terminated - *closed).count();
    const auto term_waited = duration_cast<milliseconds>(*killed - *terminated).count();
    RecordProperty("exit_grace_waited_ms", static_cast<int>(exit_waited));
    RecordProperty("term_grace_waited_ms", static_cast<int>(term_waited));
}
#endif
