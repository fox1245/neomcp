// Tests of handlers that ask their client for input, through Server and the in-memory peer: plain and coroutine
// handlers of tools, prompts and resources, under the revision whose result asks and under one that sends requests,
// and the server's time-out of a wait. Every wait is on an event a test controls; the bound on a wait only limits how
// long a broken build takes to fail.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <asio/awaitable.hpp>
#include <gtest/gtest.h>

#include "neomcp/server.h"
#include "neomcp/testing/peer.h"

namespace sit {

struct DeleteArgs {
    std::string path;
};

struct TopicArgs {
    std::string topic;
};

}  // namespace sit

NEOMCP_DESCRIBE(sit::DeleteArgs, path, "the folder to delete")
NEOMCP_DESCRIBE(sit::TopicArgs, topic, "what the prompt is about")

namespace {

using namespace std::chrono_literals;
using neomcp::json;
using neomcp::testing::Peer;
namespace profile = neomcp::profile;

constexpr std::chrono::milliseconds patience = 30s;
constexpr const char* modern_revision = "2026-07-28";
constexpr const char* legacy_revision = "2025-11-25";

json J(const char* text) { return json::parse(text); }

profile::ProfilePtr builtin(const std::string& revision) {
    auto found = profile::builtin(revision);
    if (!found) throw std::logic_error("no built-in profile " + revision);
    return found;
}

json confirm_schema() { return J(R"({"type":"object","properties":{"ok":{"type":"boolean"}},"required":["ok"]})"); }

json all_capabilities() {
    return J(R"({"elicitation":{"form":{},"url":{}},"sampling":{"tools":{},"context":{}},"roots":{}})");
}

const neomcp::ElicitationAnswer* answer_of(const neomcp::Context& ctx, const std::string& key) {
    const auto found = ctx.input_responses().find(key);
    return found == ctx.input_responses().end() ? nullptr : std::get_if<neomcp::ElicitationAnswer>(&found->second);
}

neomcp::InputRequired ask_confirm(std::string message, std::optional<std::string> state = std::nullopt) {
    neomcp::InputRequired ask;
    ask.requests.emplace("confirm", neomcp::ElicitationForm{std::move(message), confirm_schema()});
    ask.state = std::move(state);
    return ask;
}

// Counts what the handlers did, so that a test sees how often each was entered and what happened only once.
struct Effects {
    std::atomic<int> entered{0};
    std::atomic<int> deleted{0};
};

neomcp::ServerOptions options_with_key() {
    neomcp::ServerOptions options{.name = "input-server", .version = "1.0.0"};
    options.request_state_keys = {std::string(32, 's')};
    return options;
}

void register_handlers(neomcp::Server& server, Effects& effects) {
    // The pattern server.h documents: ask before acting, and act once, when the answer is there.
    server.tool<sit::DeleteArgs>(
        "delete_folder", "Delete a folder",
        [&effects](const sit::DeleteArgs& a, neomcp::Context& ctx) -> std::variant<std::string, neomcp::InputRequired> {
            ++effects.entered;
            const auto* confirm = answer_of(ctx, "confirm");
            if (confirm == nullptr) return ask_confirm("Delete " + a.path + "?", "asked:" + a.path);
            if (ctx.request_state() != std::optional<std::string>("asked:" + a.path)) return "state lost";
            if (confirm->action != neomcp::ElicitationAction::accept) return "Kept " + a.path;
            ++effects.deleted;
            return "Deleted " + a.path;
        });
    server.tool("coroutine_asks", "A coroutine that asks",
                [&effects](neomcp::Context& ctx) -> asio::awaitable<std::variant<std::string, neomcp::InputRequired>> {
                    ++effects.entered;
                    if (answer_of(ctx, "confirm") == nullptr) co_return ask_confirm("Go on?");
                    co_return std::string("went on");
                });
    server.tool("asks_what_it_can", "Asks for a form, or else for the roots",
                [](neomcp::Context& ctx) -> std::variant<std::string, neomcp::InputRequired> {
                    if (!ctx.input_responses().empty()) return "answered";
                    neomcp::InputRequired ask;
                    const neomcp::InputRequest form = neomcp::ElicitationForm{"Name?", confirm_schema()};
                    if (ctx.client_supports(form)) {
                        ask.requests.emplace("form", form);
                    } else {
                        ask.requests.emplace("roots", neomcp::RootsRequest{});
                    }
                    return ask;
                });
    server.tool("always_asks", "Never satisfied",
                [](neomcp::Context&) -> neomcp::InputRequired { return ask_confirm("Again?"); });
    neomcp::ToolDefinition raw;
    raw.name = "raw_asks";
    raw.description = "A tool written by hand that asks";
    raw.input_schema = J(R"({"type":"object"})");
    server.tool(raw, [](const json&, neomcp::Context& ctx) -> std::variant<neomcp::ToolResult, neomcp::InputRequired> {
        if (answer_of(ctx, "confirm") == nullptr) return ask_confirm("Raw?");
        neomcp::ToolResult result;
        neomcp::TextContent text;
        text.text = "raw done";
        result.content.emplace_back(std::move(text));
        return result;
    });
    server.prompt<sit::TopicArgs>("topic_prompt", "A prompt that asks",
                                  [](const sit::TopicArgs& a, neomcp::Context& ctx)
                                      -> std::variant<std::string, neomcp::InputRequired> {
                                      if (answer_of(ctx, "confirm") == nullptr) return ask_confirm("About " + a.topic + "?");
                                      return "Talk about " + a.topic;
                                  });
    server.resource("test://asks", "A resource that asks",
                    [](neomcp::Context& ctx) -> asio::awaitable<std::variant<std::string, neomcp::InputRequired>> {
                        const auto found = ctx.input_responses().find("roots");
                        if (found == ctx.input_responses().end()) {
                            neomcp::InputRequired ask;
                            ask.requests.emplace("roots", neomcp::RootsRequest{});
                            co_return ask;
                        }
                        co_return std::to_string(std::get<neomcp::RootsAnswer>(found->second).roots.size()) + " roots";
                    });
}

json accept_ok() { return J(R"({"action":"accept","content":{"ok":true}})"); }

// ---------------------------------------------------------------------------------------------------------------
// The revision whose result asks

struct ModernClient {
    explicit ModernClient(neomcp::Server& server, json capabilities = all_capabilities())
        : peer(server), profile(builtin(modern_revision)), caps(std::move(capabilities)) {}

    json ask(const std::string& method, json params) {
        params["_meta"] = Peer::request_meta(*profile, caps);
        json message = json::object();
        message["jsonrpc"] = "2.0";
        message["id"] = next_id++;
        message["method"] = method;
        message["params"] = std::move(params);
        const auto answer = peer.request(message, patience);
        if (!answer) throw std::runtime_error("no answer within the patience");
        return *answer;
    }
    // Asks once, answers what was asked with `answers`, and returns the retried call's answer.
    json round_trip(const std::string& method, json params, json answers) {
        const json first = ask(method, params);
        if (!first.contains("result") || first.at("result").at("resultType") != "input_required") return first;
        params["inputResponses"] = std::move(answers);
        params["requestState"] = first.at("result").at("requestState");
        return ask(method, std::move(params));
    }

    Peer peer;
    profile::ProfilePtr profile;
    json caps;
    int next_id = 1;
};

json tool_params(const std::string& name, json arguments = json::object()) {
    json params = json::object();
    params["name"] = name;
    params["arguments"] = std::move(arguments);
    return params;
}

std::string text_of(const json& answer) { return answer.at("result").at("content").at(0).at("text").get<std::string>(); }

TEST(ServerInput, APlainHandlerAsksAndActsOnce) {
    Effects effects;
    neomcp::Server server(options_with_key());
    register_handlers(server, effects);
    ModernClient client(server);
    const json arguments = J(R"({"path":"/tmp/x"})");
    const json first = client.ask("tools/call", tool_params("delete_folder", arguments));
    ASSERT_EQ(first.at("result").at("resultType"), "input_required") << first.dump();
    EXPECT_EQ(first.at("result").at("inputRequests").at("confirm").at("params").at("message"), "Delete /tmp/x?");
    EXPECT_EQ(effects.deleted.load(), 0) << "nothing happens before the answer";

    json params = tool_params("delete_folder", arguments);
    params["inputResponses"] = json::object();
    params["inputResponses"]["confirm"] = accept_ok();
    params["requestState"] = first.at("result").at("requestState");
    const json second = client.ask("tools/call", params);
    ASSERT_TRUE(second.contains("result")) << second.dump();
    EXPECT_EQ(text_of(second), "Deleted /tmp/x");
    EXPECT_EQ(effects.entered.load(), 2) << "entered again from its start";
    EXPECT_EQ(effects.deleted.load(), 1);

    // The state is bound to the arguments: the same answer for another folder is refused before the handler runs.
    params["arguments"] = J(R"({"path":"/home"})");
    const json other = client.ask("tools/call", params);
    ASSERT_TRUE(other.contains("error")) << other.dump();
    EXPECT_EQ(other.at("error").at("message"), "Invalid or expired requestState");
    EXPECT_EQ(effects.entered.load(), 2);
}

TEST(ServerInput, ACoroutineHandlerAPromptAndAResourceAsk) {
    Effects effects;
    neomcp::Server server(options_with_key());
    register_handlers(server, effects);
    ModernClient client(server);
    json confirm = json::object();
    confirm["confirm"] = accept_ok();
    EXPECT_EQ(text_of(client.round_trip("tools/call", tool_params("coroutine_asks"), confirm)), "went on");
    EXPECT_EQ(text_of(client.round_trip("tools/call", tool_params("raw_asks"), confirm)), "raw done");

    json prompt = json::object();
    prompt["name"] = "topic_prompt";
    prompt["arguments"] = J(R"({"topic":"tea"})");
    const json got = client.round_trip("prompts/get", prompt, confirm);
    ASSERT_TRUE(got.contains("result")) << got.dump();
    EXPECT_EQ(got.at("result").at("messages").at(0).at("content").at("text"), "Talk about tea");

    json read = json::object();
    read["uri"] = "test://asks";
    json roots = json::object();
    roots["roots"] = J(R"({"roots":[{"uri":"file:///a"},{"uri":"file:///b"}]})");
    const json contents = client.round_trip("resources/read", read, roots);
    ASSERT_TRUE(contents.contains("result")) << contents.dump();
    EXPECT_EQ(contents.at("result").at("contents").at(0).at("text"), "2 roots");
}

TEST(ServerInput, ClientSupportsLetsAHandlerAskForSomethingElse) {
    Effects effects;
    neomcp::Server server(options_with_key());
    register_handlers(server, effects);
    ModernClient forms(server, J(R"({"elicitation":{}})"));
    const json with_forms = forms.ask("tools/call", tool_params("asks_what_it_can"));
    EXPECT_TRUE(with_forms.at("result").at("inputRequests").contains("form")) << with_forms.dump();
    ModernClient roots(server, J(R"({"roots":{}})"));
    const json with_roots = roots.ask("tools/call", tool_params("asks_what_it_can"));
    EXPECT_TRUE(with_roots.at("result").at("inputRequests").contains("roots")) << with_roots.dump();
    ModernClient nothing(server, J(R"({})"));
    const json refused = nothing.ask("tools/call", tool_params("delete_folder", J(R"({"path":"/x"})")));
    ASSERT_TRUE(refused.contains("error")) << refused.dump();
    EXPECT_EQ(refused.at("error").at("code"), -32021);
}

TEST(ServerInput, TheDiagnosticHearsTheCause) {
    std::mutex mutex;
    std::vector<neomcp::RequestStateProblem> heard;
    neomcp::ServerOptions options = options_with_key();
    options.on_request_state_rejected = [&](const neomcp::RequestStateRejection& rejection) {
        const std::lock_guard<std::mutex> lock(mutex);
        heard.push_back(rejection.problem);
    };
    Effects effects;
    neomcp::Server server(options);
    register_handlers(server, effects);
    ModernClient client(server);
    json params = tool_params("coroutine_asks");
    params["requestState"] = "forged";
    const json answer = client.ask("tools/call", params);
    ASSERT_TRUE(answer.contains("error"));
    const std::lock_guard<std::mutex> lock(mutex);
    EXPECT_EQ(heard, std::vector<neomcp::RequestStateProblem>{neomcp::RequestStateProblem::malformed});
}

// ---------------------------------------------------------------------------------------------------------------
// A revision that sends requests

struct LegacyClient {
    explicit LegacyClient(neomcp::Server& server, json capabilities = all_capabilities())
        : peer(server), profile(builtin(legacy_revision)) {
        const auto opened =
            peer.request(Peer::initialize_request(*profile, neomcp::RequestId("init"), std::move(capabilities)), patience);
        EXPECT_TRUE(opened && opened->contains("result"));
        peer.notify(J(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
    }
    void send_call(int id, json params) {
        json message = json::object();
        message["jsonrpc"] = "2.0";
        message["id"] = id;
        message["method"] = "tools/call";
        message["params"] = std::move(params);
        peer.send(neomcp::serialize(message));
    }
    void answer(const json& request, json result) {
        json response = json::object();
        response["jsonrpc"] = "2.0";
        response["id"] = request.at("id");
        response["result"] = std::move(result);
        peer.send(neomcp::serialize(response));
    }

    Peer peer;
    profile::ProfilePtr profile;
};

TEST(ServerInput, HandlersAreServedThroughRequestsWhereTheRevisionSendsThem) {
    Effects effects;
    neomcp::Server server(options_with_key());
    register_handlers(server, effects);
    LegacyClient client(server);
    for (const char* tool : {"coroutine_asks", "raw_asks"}) {
        client.send_call(1, tool_params(tool));
        const auto request = client.peer.next_request(patience);
        ASSERT_TRUE(request) << tool;
        EXPECT_EQ(request->at("method"), "elicitation/create");
        client.answer(*request, accept_ok());
        const auto done = client.peer.await_response(neomcp::RequestId(1), patience);
        ASSERT_TRUE(done) << tool;
        EXPECT_FALSE(done->at("result").value("isError", false)) << done->dump();
    }
    client.send_call(2, tool_params("delete_folder", J(R"({"path":"/srv"})")));
    const auto request = client.peer.next_request(patience);
    ASSERT_TRUE(request);
    client.answer(*request, J(R"({"action":"decline"})"));
    const auto done = client.peer.await_response(neomcp::RequestId(2), patience);
    ASSERT_TRUE(done);
    EXPECT_EQ(text_of(*done), "Kept /srv") << "the handler's state came back in memory";
    EXPECT_EQ(effects.deleted.load(), 0);
}

TEST(ServerInput, AWaitTimesOut) {
    neomcp::ServerOptions options = options_with_key();
    options.input_timeout = 100ms;
    Effects effects;
    neomcp::Server server(options);
    register_handlers(server, effects);
    LegacyClient client(server);
    client.send_call(1, tool_params("coroutine_asks"));
    const auto request = client.peer.next_request(patience);
    ASSERT_TRUE(request);
    // Nobody answers: the server's timer ends the wait.
    const auto done = client.peer.await_response(neomcp::RequestId(1), patience);
    ASSERT_TRUE(done);
    EXPECT_TRUE(done->at("result").at("isError").get<bool>()) << done->dump();
    client.answer(*request, accept_ok());  // too late: ignored
    EXPECT_TRUE(client.peer.request(J(R"({"jsonrpc":"2.0","id":"p","method":"ping"})"), patience));
    EXPECT_EQ(effects.entered.load(), 1);
}

TEST(ServerInput, TheRoundLimitAppliesToHandlers) {
    neomcp::ServerOptions options = options_with_key();
    options.max_input_rounds = 2;
    Effects effects;
    neomcp::Server server(options);
    register_handlers(server, effects);
    LegacyClient client(server);
    client.send_call(1, tool_params("always_asks"));
    int asked = 0;
    for (;;) {
        if (auto done = client.peer.await_response(neomcp::RequestId(1), 0ms)) {
            EXPECT_TRUE(done->at("result").at("isError").get<bool>()) << done->dump();
            break;
        }
        // The next thing the session writes is either a request or the call's answer.
        const auto request = client.peer.next_request(200ms);
        if (!request) continue;
        ++asked;
        ASSERT_LE(asked, 2);
        client.answer(*request, accept_ok());
    }
    EXPECT_EQ(asked, 2);
}

// A server destroyed while a call waits for its client must not wait for the time-out.
TEST(ServerInput, DestroyingTheServerWhileACallWaitsDoesNotWaitForTheTimeOut) {
    Effects effects;
    auto server = std::make_unique<neomcp::Server>(options_with_key());  // the default time-out: ten minutes
    register_handlers(*server, effects);
    LegacyClient client(*server);
    client.send_call(1, tool_params("coroutine_asks"));
    ASSERT_TRUE(client.peer.next_request(patience));
    std::mutex mutex;
    std::condition_variable changed;
    bool destroyed = false;
    std::thread destroyer([&] {
        server.reset();
        const std::lock_guard<std::mutex> lock(mutex);
        destroyed = true;
        changed.notify_all();
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        EXPECT_TRUE(changed.wait_for(lock, patience, [&] { return destroyed; }));
    }
    destroyer.join();
    // The session outlives the server; closing it ends the wait.
    client.peer.close();
}

TEST(ServerInput, BadOptionsAreRefused) {
    neomcp::ServerOptions no_time = options_with_key();
    no_time.input_timeout = 0ms;
    EXPECT_THROW(neomcp::Server{no_time}, std::invalid_argument);
    neomcp::ServerOptions no_rounds = options_with_key();
    no_rounds.max_input_rounds = 0;
    EXPECT_THROW(neomcp::Server{no_rounds}, std::invalid_argument);
    neomcp::ServerOptions short_key = options_with_key();
    short_key.request_state_keys = {"short"};
    EXPECT_THROW(neomcp::Server{short_key}, std::invalid_argument);
}

}  // namespace
