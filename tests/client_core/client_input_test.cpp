// The client's input handlers and their dispatcher: against the server's asking tools on every revision through the
// pair with the session, and against a scripted server where a test needs a server that misbehaves.
#include <gtest/gtest.h>

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>

#include "engine_rig.h"
#include "neomcp/client/input_dispatch.h"
#include "neomcp/client/input_handlers.h"
#include "neomcp/server.h"
#include "neomcp/wire_read.h"
#include "pair.h"

namespace {

using namespace client_core_test;
using neomcp::CallFailureKind;
using neomcp::DeadlineKind;
using neomcp::DiagnosticKind;
using neomcp::ElicitationAction;
using neomcp::ElicitationAnswer;
using neomcp::InputContext;
using neomcp::OutgoingKind;
using neomcp::profile::Transport;

json J(const char* text) { return json::parse(text); }

constexpr std::string_view handshake_revision = "2025-11-25";
constexpr std::string_view per_request_revision = "2026-07-28";

// ---------------------------------------------------------------------------------------------------------------
// Runners

neomcp::HandlerOutcome run_job(neomcp::InputJob& job) {
    if (job.plain) return job.plain();
    asio::io_context io;
    std::optional<neomcp::HandlerOutcome> outcome;
    asio::co_spawn(io, job.coroutine(), [&](const std::exception_ptr&, neomcp::HandlerOutcome value) {
        outcome = std::move(value);
    });
    io.run();
    if (!outcome) return neomcp::HandlerFailed{"the coroutine did not finish"};
    return std::move(*outcome);
}

// Runs every job at once, inside start.
class SyncRunner final : public neomcp::InputRunner {
public:
    void start(neomcp::InputJob job) override {
        ++started;
        dispatcher->complete(job.id, run_job(job));
    }
    neomcp::InputDispatcher* dispatcher = nullptr;
    std::size_t started = 0;
};

// Holds every job until the test runs it.
class DeferredRunner final : public neomcp::InputRunner {
public:
    void start(neomcp::InputJob job) override { jobs.push_back(std::move(job)); }
    void run(std::size_t index) { dispatcher->complete(jobs.at(index).id, run_job(jobs.at(index))); }
    void complete(std::size_t index, neomcp::HandlerOutcome outcome) {
        dispatcher->complete(jobs.at(index).id, std::move(outcome));
    }
    neomcp::InputDispatcher* dispatcher = nullptr;
    std::vector<neomcp::InputJob> jobs;
};

// A dispatcher fed with the events of a rig's engine.
class Feed {
public:
    Feed(Rig& rig, neomcp::InputHandlers handlers, neomcp::InputRunner& runner)
        : rig_(rig), dispatcher(*rig.engine, std::move(handlers), runner) {}

    // Hands every event the engine queued since the last time to the dispatcher, until none is left.
    void operator()() {
        while (seen_ < rig_.drain().size()) {
            for (; seen_ < rig_.log.size(); ++seen_) {
                const neomcp::EngineEvent event = rig_.log[seen_];
                dispatcher.take(event);
            }
        }
    }

    std::vector<neomcp::InputReport> reports() {
        while (auto report = dispatcher.next_report()) taken_.push_back(std::move(*report));
        return taken_;
    }

private:
    Rig& rig_;
    std::size_t seen_ = 0;
    std::vector<neomcp::InputReport> taken_;

public:
    neomcp::InputDispatcher dispatcher;
};

// ---------------------------------------------------------------------------------------------------------------
// What the revisions have

neomcp::profile::ProfilePtr profile_of(std::string_view revision) { return neomcp::profile::builtin(revision); }

bool server_asks(const neomcp::profile::Profile& profile, std::string_view method) {
    const auto* sent = profile.request(method);
    return profile.input_request(method) != nullptr || (sent != nullptr && sent->from_server);
}

bool has_url_mode(const neomcp::profile::Profile& profile) { return profile.type("ElicitRequestURLParams") != nullptr; }

neomcp::InputDelivery delivery_of(const neomcp::profile::Profile& profile) {
    return neomcp::builtin_client_mechanisms()
        .make_input(profile.mechanism(neomcp::profile::Slot::server_input))
        ->delivery();
}

// A form that the revision's primitive schema definitions take: a string with a default where the revision gives a
// string a default, and a required boolean.
json form_schema(const neomcp::profile::Profile& profile) {
    json name = J(R"({"type":"string","title":"Name"})");
    const auto* string_type = profile.type("StringSchema");
    if (string_type != nullptr && string_type->has_property("default")) name["default"] = "Ada";
    json schema = J(R"({"type":"object","properties":{"ok":{"type":"boolean"}},"required":["ok"]})");
    schema["properties"]["name"] = std::move(name);
    return schema;
}

neomcp::SamplingRequest sampling_request() {
    neomcp::SamplingRequest request;
    neomcp::SamplingMessage message;
    neomcp::TextContent text;
    text.text = "hi";
    message.content.emplace_back(text);
    request.messages.push_back(std::move(message));
    request.max_tokens = 10;
    return request;
}

neomcp::SamplingRequest sampling_with_tools() {
    neomcp::SamplingRequest request = sampling_request();
    neomcp::ToolDefinition tool;
    tool.name = "lookup";
    tool.input_schema = J(R"({"type":"object"})");
    request.tools.push_back(std::move(tool));
    request.include_context = neomcp::IncludeContext::this_server;
    return request;
}

// ---------------------------------------------------------------------------------------------------------------
// The server's asking tools

template <class T>
const T* answer_of(const neomcp::Context& ctx, const std::string& key) {
    const auto found = ctx.input_responses().find(key);
    return found == ctx.input_responses().end() ? nullptr : std::get_if<T>(&found->second);
}

using Asked = std::variant<std::string, neomcp::InputRequired>;

neomcp::InputRequired asking(std::string key, neomcp::InputRequest request) {
    neomcp::InputRequired ask;
    ask.requests.emplace(std::move(key), std::move(request));
    return ask;
}

std::string action_name(ElicitationAction action) {
    switch (action) {
        case ElicitationAction::accept: return "accept";
        case ElicitationAction::decline: return "decline";
        case ElicitationAction::cancel: return "cancel";
    }
    return "?";
}

void populate_asking(neomcp::Server& server) {
    server.tool("ask_form", "Asks for a form", [](neomcp::Context& ctx) -> Asked {
        if (const auto* answer = answer_of<ElicitationAnswer>(ctx, "f")) {
            return action_name(answer->action) + " " + answer->content.dump();
        }
        if (ctx.input_responses().count("f") != 0) return "answered otherwise";
        return asking("f", neomcp::ElicitationForm{"Who?", form_schema(*neomcp::profile::builtin(ctx.revision()))});
    });
    server.tool("ask_nested", "Asks for a form outside the restricted form", [](neomcp::Context& ctx) -> Asked {
        if (!ctx.input_responses().empty()) return "answered";
        return asking("f", neomcp::ElicitationForm{
                               "Nested?", J(R"({"type":"object","properties":{"x":{"type":"object",
                               "properties":{"y":{"type":"string"}}}}})")});
    });
    server.tool("ask_url", "Asks to visit a URL", [](neomcp::Context& ctx) -> Asked {
        if (const auto* answer = answer_of<ElicitationAnswer>(ctx, "u")) return "url " + action_name(answer->action);
        return asking("u", neomcp::ElicitationUrl{"Sign in", "https://example.com/sign-in?x=1", std::nullopt});
    });
    server.tool("ask_sampling", "Asks for a completion", [](neomcp::Context& ctx) -> Asked {
        if (const auto* answer = answer_of<neomcp::SamplingAnswer>(ctx, "s")) return "model " + answer->model;
        if (ctx.input_responses().count("s") != 0) return "answered otherwise";
        return asking("s", sampling_request());
    });
    server.tool("ask_sampling_tools", "Asks for a completion with tools where the client takes them",
                [](neomcp::Context& ctx) -> Asked {
                    if (const auto* answer = answer_of<neomcp::SamplingAnswer>(ctx, "s")) return "model " + answer->model;
                    const neomcp::InputRequest request = sampling_with_tools();
                    if (!ctx.client_supports(request)) return "tools not supported";
                    return asking("s", request);
                });
    server.tool("ask_roots", "Asks for the roots", [](neomcp::Context& ctx) -> Asked {
        if (const auto* answer = answer_of<neomcp::RootsAnswer>(ctx, "r")) return "root " + answer->roots.at(0).uri;
        return asking("r", neomcp::RootsRequest{});
    });
    server.tool("ask_mix", "Asks for a form, a completion and the roots at once", [](neomcp::Context& ctx) -> Asked {
        const auto* form = answer_of<ElicitationAnswer>(ctx, "a");
        const auto* sampled = answer_of<neomcp::SamplingAnswer>(ctx, "b");
        const auto* roots = answer_of<neomcp::RootsAnswer>(ctx, "c");
        if (form != nullptr && sampled != nullptr && roots != nullptr) {
            return action_name(form->action) + " " + sampled->model + " " + roots->roots.at(0).uri;
        }
        neomcp::InputRequired ask;
        ask.requests.emplace("a", neomcp::ElicitationForm{"Who?", form_schema(*neomcp::profile::builtin(ctx.revision()))});
        ask.requests.emplace("b", sampling_request());
        ask.requests.emplace("c", neomcp::RootsRequest{});
        return ask;
    });
    server.tool("ask_rounds", "Asks for a form in three rounds", [](neomcp::Context& ctx) -> Asked {
        const int round = ctx.request_state() ? std::stoi(*ctx.request_state()) : 0;
        if (round >= 3) return "rounds done";
        neomcp::InputRequired ask = asking(
            "q", neomcp::ElicitationForm{"Again?", form_schema(*neomcp::profile::builtin(ctx.revision()))});
        ask.state = std::to_string(round + 1);
        return ask;
    });
    server.tool("ask_three_forms", "Asks for three forms in one round", [](neomcp::Context& ctx) -> Asked {
        if (!ctx.input_responses().empty()) return "answered";
        neomcp::InputRequired ask;
        for (const char* key : {"k1", "k2", "k3"}) {
            ask.requests.emplace(key, neomcp::ElicitationForm{"Who?", form_schema(*neomcp::profile::builtin(ctx.revision()))});
        }
        return ask;
    });
}

neomcp::ServerOptions asking_server() {
    neomcp::ServerOptions options;
    options.name = "asking-server";
    options.version = "1.0.0";
    options.request_state_keys = {std::string(32, 'k')};
    return options;
}

// ---------------------------------------------------------------------------------------------------------------
// The pair with a dispatcher

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

// A client of one revision with `handlers`, against the asking server, with a dispatcher fed on every turn.
class InputPair {
public:
    InputPair(const Cell& cell, const neomcp::InputHandlers& handlers, neomcp::InputRunner& runner,
              const std::function<void(neomcp::EngineOptions&)>& adjust = {})
        : pair_(options(cell, handlers, adjust), cell.transport, asking_server(), populate_asking),
          feed(pair_.rig(), handlers, runner) {
        pair_.on_turn = [this] { feed(); };
        pair_.open();
    }

    Rig& rig() { return pair_.rig(); }

    neomcp::RequestEnded call(const std::string& tool) {
        return pair_.call("tools/call", json{{"name", tool}, {"arguments", json::object()}});
    }

    bool pump(const std::function<bool()>& done) { return pair_.pump(done); }

    // The text of a tool result.
    std::string text(const neomcp::RequestEnded& ended) {
        const auto* answered = std::get_if<neomcp::Answered>(&ended.outcome);
        if (answered == nullptr) return "(no result)";
        auto read = neomcp::from_wire<neomcp::ToolResult>(answered->result, pair_.profile());
        if (!std::holds_alternative<neomcp::ToolResult>(read)) return "(unreadable)";
        const auto& result = std::get<neomcp::ToolResult>(read);
        if (result.content.empty()) return "(empty)";
        const auto* first = std::get_if<neomcp::TextContent>(&result.content[0]);
        return first == nullptr ? "(not text)" : first->text;
    }

    // The client's responses to the server's requests, parsed.
    std::vector<json> responses() {
        std::vector<json> found;
        for (const auto& outgoing : rig().outputs(OutgoingKind::response)) found.push_back(json::parse(outgoing.line));
        return found;
    }

    // Whether any message the client wrote holds `text`.
    bool wrote(std::string_view text) {
        for (const auto& outgoing : rig().out) {
            if (outgoing.line.find(text) != std::string::npos) return true;
        }
        return false;
    }

    std::size_t count(std::string_view method) {
        std::size_t found = 0;
        for (const auto& request : rig().requests()) found += request["method"] == std::string(method) ? 1 : 0;
        return found;
    }

private:
    static neomcp::EngineOptions options(const Cell& cell, const neomcp::InputHandlers& handlers,
                                         const std::function<void(neomcp::EngineOptions&)>& adjust) {
        auto made = only({cell.revision});
        made.input = handlers.support();
        if (adjust) adjust(made);
        return made;
    }

    Pair pair_;

public:
    Feed feed;
};

// The code of an error response; zero for a response that is no error.
int error_code(const json& response) {
    if (!response.is_object() || !response.contains("error")) return 0;
    const json& error = response["error"];
    return error.contains("code") && error["code"].is_number_integer() ? error["code"].get<int>() : 0;
}

neomcp::CallFailureKind failure_of(const neomcp::RequestEnded& ended) {
    const auto* failure = std::get_if<neomcp::CallFailure>(&ended.outcome);
    if (failure == nullptr) throw std::runtime_error("the call did not fail");
    return failure->kind;
}

ElicitationAnswer accepting(json content) { return ElicitationAnswer{ElicitationAction::accept, std::move(content)}; }

neomcp::SamplingAnswer completion(std::string model) {
    neomcp::SamplingAnswer answer;
    neomcp::TextContent said;
    said.text = "hello";
    answer.content.push_back(std::move(said));
    answer.model = std::move(model);
    return answer;
}

neomcp::RootsAnswer work_root() {
    neomcp::RootsAnswer answer;
    answer.roots.push_back(neomcp::Root{"file:///work", std::string("work"), std::nullopt});
    return answer;
}

class ClientInputPair : public testing::TestWithParam<Cell> {
protected:
    neomcp::profile::ProfilePtr profile() const { return profile_of(GetParam().revision); }
    bool legacy() const { return delivery_of(*profile()) == neomcp::InputDelivery::requests; }
};

TEST_P(ClientInputPair, AFormReachesTheHandlerWithItsDefaultsAndItsAnswerCompletesTheCall) {
    if (!server_asks(*profile(), "elicitation/create")) {
        GTEST_SKIP() << "the revision cannot ask for a form";
    }
    std::optional<json> offered;
    std::optional<std::string> seen_revision;
    neomcp::InputHandlers handlers;
    handlers.form([&](const neomcp::ElicitationForm& form, InputContext& context) {
        offered = form.prefilled();
        seen_revision = context.revision();
        json content = form.prefilled();
        content["ok"] = true;
        if (!content.contains("name")) content["name"] = "Bo";
        return accepting(std::move(content));
    });
    SyncRunner runner;
    InputPair pair(GetParam(), handlers, runner);
    runner.dispatcher = &pair.feed.dispatcher;
    const auto ended = pair.call("ask_form");
    ASSERT_TRUE(offered);
    EXPECT_EQ(seen_revision, GetParam().revision);
    const bool defaults = form_schema(*profile())["properties"]["name"].contains("default");
    EXPECT_EQ(*offered, defaults ? J(R"({"name":"Ada"})") : json::object());
    const std::string text = pair.text(ended);
    EXPECT_EQ(text.rfind("accept ", 0), 0u) << text;
    EXPECT_NE(text.find(defaults ? "Ada" : "Bo"), std::string::npos) << text;
    EXPECT_EQ(runner.started, 1u);
    EXPECT_EQ(pair.feed.dispatcher.outstanding(), 0u);
}

TEST_P(ClientInputPair, AUrlIsHandedToTheHandlerAsDataWhereTheRevisionHasUrls) {
    if (!has_url_mode(*profile())) {
        GTEST_SKIP() << "the revision has no elicitation in URL mode";
    }
    std::optional<std::string> url;
    neomcp::InputHandlers handlers;
    handlers.url([&](neomcp::ElicitationUrl request) -> ElicitationAnswer {
        url = request.url;
        return ElicitationAnswer{ElicitationAction::decline, json::object()};
    });
    SyncRunner runner;
    InputPair pair(GetParam(), handlers, runner);
    runner.dispatcher = &pair.feed.dispatcher;
    const auto ended = pair.call("ask_url");
    EXPECT_EQ(url, std::optional<std::string>("https://example.com/sign-in?x=1"));
    EXPECT_EQ(pair.text(ended), "url decline");
}

TEST_P(ClientInputPair, ASamplingRequestIsAnsweredByTheHandler) {
    if (!server_asks(*profile(), "sampling/createMessage")) {
        GTEST_SKIP() << "the revision has no sampling";
    }
    neomcp::InputHandlers handlers;
    handlers.sampling([](const neomcp::SamplingRequest& request) {
        return completion("model-" + std::to_string(request.max_tokens));
    });
    SyncRunner runner;
    InputPair pair(GetParam(), handlers, runner);
    runner.dispatcher = &pair.feed.dispatcher;
    EXPECT_EQ(pair.text(pair.call("ask_sampling")), "model model-10");
}

TEST_P(ClientInputPair, SamplingWithToolsReachesOnlyAHandlerThatDeclaresThem) {
    if (!server_asks(*profile(), "sampling/createMessage")) {
        GTEST_SKIP() << "the revision has no sampling";
    }
    const auto* params = profile()->type("CreateMessageRequestParams");
    const bool revision_has_tools = params != nullptr && params->has_property("tools");
    for (const bool declared : {false, true}) {
        SCOPED_TRACE(declared ? "tools declared" : "tools not declared");
        std::size_t tools_seen = 0;
        neomcp::InputHandlers handlers;
        handlers.sampling(
            [&](neomcp::SamplingRequest request, InputContext&) -> asio::awaitable<neomcp::SamplingAnswer> {
                tools_seen = request.tools.size();
                co_return completion("with-tools");
            },
            neomcp::SamplingSupport{declared, declared});
        SyncRunner runner;
        InputPair pair(GetParam(), handlers, runner);
        runner.dispatcher = &pair.feed.dispatcher;
        const std::string text = pair.text(pair.call("ask_sampling_tools"));
        if (declared && revision_has_tools) {
            EXPECT_EQ(text, "model with-tools");
            EXPECT_EQ(tools_seen, 1u);
        } else {
            EXPECT_EQ(text, "tools not supported");
            EXPECT_EQ(runner.started, 0u);
        }
    }
}

TEST_P(ClientInputPair, TheRootsAreAnsweredByACoroutineHandler) {
    if (!server_asks(*profile(), "roots/list")) {
        GTEST_SKIP() << "the revision has no roots";
    }
    neomcp::InputHandlers handlers;
    handlers.roots([]() -> asio::awaitable<neomcp::RootsAnswer> { co_return work_root(); });
    SyncRunner runner;
    InputPair pair(GetParam(), handlers, runner);
    runner.dispatcher = &pair.feed.dispatcher;
    EXPECT_EQ(pair.text(pair.call("ask_roots")), "root file:///work");
}

TEST_P(ClientInputPair, SeveralRequestsOfOneRoundRunTogetherAndAnswerInAnyOrder) {
    const auto& p = *profile();
    if (!server_asks(p, "elicitation/create") || !server_asks(p, "sampling/createMessage") ||
        !server_asks(p, "roots/list")) {
        GTEST_SKIP() << "the revision cannot ask for all three";
    }
    neomcp::InputHandlers handlers;
    handlers.form([](const neomcp::ElicitationForm&) { return accepting(J(R"({"ok":true})")); })
        .sampling([](const neomcp::SamplingRequest&) { return completion("m"); })
        .roots([](InputContext&) { return work_root(); });
    DeferredRunner runner;
    InputPair pair(GetParam(), handlers, runner);
    runner.dispatcher = &pair.feed.dispatcher;
    const auto ref = pair.rig().send("tools/call", json{{"name", "ask_mix"}, {"arguments", json::object()}});
    ASSERT_TRUE(pair.pump([&] { return runner.jobs.size() == 3 || pair.rig().ended(ref).has_value(); }));
    ASSERT_EQ(runner.jobs.size(), 3u);
    EXPECT_EQ(pair.feed.dispatcher.outstanding(), 3u);
    // Nothing goes back before the last of them answered.
    runner.run(2);
    runner.run(1);
    if (!legacy()) {
        EXPECT_EQ(pair.count("tools/call"), 1u);
    }
    runner.run(0);
    std::optional<neomcp::RequestEnded> ended;
    ASSERT_TRUE(pair.pump([&] { return (ended = pair.rig().ended(ref)).has_value(); }));
    EXPECT_EQ(pair.text(*ended), "accept m file:///work");
    EXPECT_EQ(pair.feed.dispatcher.outstanding(), 0u);
}

TEST_P(ClientInputPair, ARoundPastTheBoundEndsTheCallOrIsRefused) {
    if (!server_asks(*profile(), "elicitation/create")) {
        GTEST_SKIP() << "the revision cannot ask for a form";
    }
    if (legacy() && GetParam().transport == Transport::stdio) {
        GTEST_SKIP() << "on stdio the wire does not say which call a server's request belongs to";
    }
    std::atomic<int> runs{0};
    neomcp::InputHandlers handlers;
    handlers.form([&](const neomcp::ElicitationForm&) {
        ++runs;
        return accepting(J(R"({"ok":true})"));
    });
    SyncRunner runner;
    InputPair pair(GetParam(), handlers, runner, [](neomcp::EngineOptions& options) { options.max_input_rounds = 2; });
    runner.dispatcher = &pair.feed.dispatcher;
    const auto ended = pair.call("ask_rounds");
    EXPECT_EQ(runs.load(), 2);
    if (legacy()) {
        // The third request of the server was answered with an internal error and reached no handler.
        bool refused = false;
        for (const auto& response : pair.responses()) {
            refused = refused || error_code(response) == -32603;
        }
        EXPECT_TRUE(refused);
    } else {
        EXPECT_EQ(failure_of(ended), CallFailureKind::input_rounds_exceeded);
    }
}

TEST_P(ClientInputPair, MoreRequestsInOneRoundThanTheBoundEndTheCall) {
    if (!server_asks(*profile(), "elicitation/create")) {
        GTEST_SKIP() << "the revision cannot ask for a form";
    }
    if (legacy()) {
        GTEST_SKIP() << "a server's own requests are bounded by max_concurrent_server_requests instead";
    }
    neomcp::InputHandlers handlers;
    handlers.form([](const neomcp::ElicitationForm&) { return accepting(J(R"({"ok":true})")); });
    SyncRunner runner;
    InputPair pair(GetParam(), handlers, runner,
                   [](neomcp::EngineOptions& options) { options.max_input_requests_per_round = 2; });
    runner.dispatcher = &pair.feed.dispatcher;
    const auto ended = pair.call("ask_three_forms");
    EXPECT_EQ(failure_of(ended), CallFailureKind::input_requests_exceeded);
    EXPECT_EQ(runner.started, 0u);
    EXPECT_EQ(pair.count("tools/call"), 1u);
}

TEST_P(ClientInputPair, AFormOutsideTheRestrictedFormNeverReachesTheHandler) {
    if (!server_asks(*profile(), "elicitation/create")) {
        GTEST_SKIP() << "the revision cannot ask for a form";
    }
    neomcp::InputHandlers handlers;
    handlers.form([](const neomcp::ElicitationForm&) { return accepting(J(R"({"x":{"y":"z"}})")); });
    SyncRunner runner;
    InputPair pair(GetParam(), handlers, runner);
    runner.dispatcher = &pair.feed.dispatcher;
    const auto ended = pair.call("ask_nested");
    EXPECT_EQ(runner.started, 0u);
    if (legacy()) {
        const auto responses = pair.responses();
        ASSERT_EQ(responses.size(), 1u);
        EXPECT_EQ(error_code(responses[0]), -32602);
        EXPECT_EQ(pair.rig().diagnostics(DiagnosticKind::form_not_restricted).size(), 1u);
    } else {
        EXPECT_EQ(failure_of(ended), CallFailureKind::form_not_restricted);
        EXPECT_EQ(pair.count("tools/call"), 1u);
    }
}

TEST_P(ClientInputPair, AcceptedContentThatFailsItsSchemaIsReportedAndNeverSent) {
    if (!server_asks(*profile(), "elicitation/create")) {
        GTEST_SKIP() << "the revision cannot ask for a form";
    }
    neomcp::InputHandlers handlers;
    handlers.form([](const neomcp::ElicitationForm&) { return accepting(J(R"({"ok":"not-a-boolean"})")); });
    SyncRunner runner;
    InputPair pair(GetParam(), handlers, runner);
    runner.dispatcher = &pair.feed.dispatcher;
    const auto ended = pair.call("ask_form");
    EXPECT_FALSE(pair.wrote("not-a-boolean"));
    if (legacy()) {
        const auto responses = pair.responses();
        ASSERT_EQ(responses.size(), 1u);
        EXPECT_EQ(error_code(responses[0]), -32603);
        const auto reported = pair.rig().diagnostics(DiagnosticKind::answer_invalid);
        ASSERT_EQ(reported.size(), 1u);
        EXPECT_TRUE(reported[0].input.has_value());
    } else {
        EXPECT_EQ(failure_of(ended), CallFailureKind::answer_invalid);
        EXPECT_EQ(pair.count("tools/call"), 1u);
    }
}

TEST_P(ClientInputPair, AHandlerThatThrowsGivesAFixedErrorAndItsTextStaysWithTheApplication) {
    if (!server_asks(*profile(), "elicitation/create")) {
        GTEST_SKIP() << "the revision cannot ask for a form";
    }
    neomcp::InputHandlers handlers;
    handlers.form([](const neomcp::ElicitationForm&) -> ElicitationAnswer {
        throw std::runtime_error("private-detail-of-the-handler");
    });
    SyncRunner runner;
    InputPair pair(GetParam(), handlers, runner);
    runner.dispatcher = &pair.feed.dispatcher;
    const auto ended = pair.call("ask_form");
    EXPECT_FALSE(pair.wrote("private-detail-of-the-handler"));
    const auto reports = pair.feed.reports();
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].kind, neomcp::InputReportKind::handler_failed);
    EXPECT_EQ(reports[0].message, "private-detail-of-the-handler");
    if (legacy()) {
        const auto responses = pair.responses();
        ASSERT_EQ(responses.size(), 1u);
        EXPECT_EQ(error_code(responses[0]), -32603);
    } else {
        EXPECT_EQ(failure_of(ended), CallFailureKind::handler_failed);
        EXPECT_EQ(pair.count("tools/call"), 1u);
    }
}

TEST_P(ClientInputPair, TheContextAndAReportNameTheCallTheWireTiesTheRequestTo) {
    if (!server_asks(*profile(), "elicitation/create")) {
        GTEST_SKIP() << "the revision cannot ask for a form";
    }
    std::optional<std::optional<neomcp::RequestRef>> seen;
    neomcp::InputHandlers handlers;
    handlers.form([&](const neomcp::ElicitationForm&, InputContext& context) -> ElicitationAnswer {
        seen = context.call();
        throw std::runtime_error("refused");
    });
    SyncRunner runner;
    InputPair pair(GetParam(), handlers, runner);
    runner.dispatcher = &pair.feed.dispatcher;
    const auto ref = pair.rig().send("tools/call", json{{"name", "ask_form"}, {"arguments", json::object()}});
    ASSERT_TRUE(pair.pump([&] { return pair.rig().ended(ref).has_value(); }));
    ASSERT_TRUE(seen);
    // The wire ties a server's request to a call on that call's own exchange (HTTP), and a round always.
    const bool tied = !legacy() || GetParam().transport == Transport::streamable_http;
    const std::optional<neomcp::RequestRef> expected = tied ? std::optional<neomcp::RequestRef>(ref) : std::nullopt;
    EXPECT_EQ(*seen, expected);
    const auto reports = pair.feed.reports();
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].call, expected);
}

TEST_P(ClientInputPair, AUserWhoRefusesSamplingIsReportedAsTheRevisionPrescribes) {
    if (!server_asks(*profile(), "sampling/createMessage")) {
        GTEST_SKIP() << "the revision has no sampling";
    }
    neomcp::InputHandlers handlers;
    handlers.sampling([](const neomcp::SamplingRequest&) -> neomcp::SamplingReply { return neomcp::InputRefused{}; });
    SyncRunner runner;
    InputPair pair(GetParam(), handlers, runner);
    runner.dispatcher = &pair.feed.dispatcher;
    const auto ended = pair.call("ask_sampling");
    if (legacy()) {
        const auto responses = pair.responses();
        ASSERT_EQ(responses.size(), 1u);
        EXPECT_EQ(error_code(responses[0]), -1);
    } else {
        EXPECT_EQ(failure_of(ended), CallFailureKind::input_refused);
        EXPECT_EQ(pair.count("tools/call"), 1u);
    }
}

INSTANTIATE_TEST_SUITE_P(ClientInputEveryRevision, ClientInputPair, testing::ValuesIn(cells()), cell_name);

// ---------------------------------------------------------------------------------------------------------------
// A scripted server

neomcp::EngineOptions opened(std::string_view revision, const neomcp::InputSupport& support) {
    auto options = only({revision});
    options.input = support;
    const auto profile = profile_of(revision);
    const bool per_request = delivery_of(*profile) == neomcp::InputDelivery::result;
    options.prior = neomcp::ContactVerdict{per_request ? neomcp::ServerKind::per_request : neomcp::ServerKind::handshake,
                                           Transport::stdio, std::string(revision)};
    return options;
}

// A rig whose contact is open at `revision` over stdio.
void open(Rig& rig, std::string_view revision) {
    rig.engine->open();
    if (rig.engine->contact().phase == neomcp::ContactPhase::handshaking) {
        rig.answer("initialize", initialize_result(revision));
    }
    if (rig.engine->contact().phase != neomcp::ContactPhase::open) throw std::runtime_error("contact did not open");
}

// The last response the client wrote, parsed.
json last_response(const Rig& rig) {
    const auto responses = rig.outputs(OutgoingKind::response);
    if (responses.empty()) throw std::runtime_error("no response was written");
    return json::parse(responses.back().line);
}

void server_request(Rig& rig, int id, std::string_view method, const json& params) {
    json message = json{{"jsonrpc", "2.0"}, {"id", id}, {"method", std::string(method)}, {"params", params}};
    rig.engine->received(neomcp::connection_unit, neomcp::serialize(message));
}

json form_params(const char* schema) {
    return json{{"mode", "form"}, {"message", "Sure?"}, {"requestedSchema", J(schema)}};
}

json sampling_params() {
    return J(R"({"messages":[{"role":"user","content":{"type":"text","text":"hi"}}],"maxTokens":5})");
}

// A result that asks with these requests, keyed.
json asking_result(const json& requests) {
    return json{{"resultType", "input_required"}, {"inputRequests", requests}, {"requestState", "st"}};
}

neomcp::RequestRef tool_call(Rig& rig) {
    return rig.send("tools/call", json{{"name", "t"}, {"arguments", json::object()}});
}

TEST(ClientInputRefusal, AKindWithoutAHandlerIsAMissingMethodAndWhatAKindLacksIsInvalidParams) {
    // No handler at all: every kind, elicitation included, is a method the client does not have.
    {
        neomcp::InputHandlers none;
        Rig rig(opened(handshake_revision, none.support()));
        open(rig, handshake_revision);
        server_request(rig, 1, "elicitation/create", form_params(R"({"type":"object","properties":{}})"));
        EXPECT_EQ(error_code(last_response(rig)), -32601);
        server_request(rig, 2, "sampling/createMessage", sampling_params());
        EXPECT_EQ(error_code(last_response(rig)), -32601);
        server_request(rig, 3, "roots/list", json::object());
        EXPECT_EQ(error_code(last_response(rig)), -32601);
    }
    // A form handler and a sampling handler without tools or context: a URL, sampling with tools, and sampling with
    // context are what the client did not declare within kinds it declares.
    {
        neomcp::InputHandlers some;
        some.form([](const neomcp::ElicitationForm&) { return accepting(json::object()); })
            .sampling([](const neomcp::SamplingRequest&) { return completion("m"); });
        Rig rig(opened(handshake_revision, some.support()));
        open(rig, handshake_revision);
        server_request(rig, 1, "elicitation/create",
                       J(R"({"mode":"url","message":"Go","url":"https://example.com","elicitationId":"e1"})"));
        EXPECT_EQ(error_code(last_response(rig)), -32602);
        json with_tools = sampling_params();
        with_tools["tools"] = J(R"([{"name":"lookup","inputSchema":{"type":"object"}}])");
        server_request(rig, 2, "sampling/createMessage", with_tools);
        EXPECT_EQ(error_code(last_response(rig)), -32602);
        json with_context = sampling_params();
        with_context["includeContext"] = "allServers";
        server_request(rig, 3, "sampling/createMessage", with_context);
        EXPECT_EQ(error_code(last_response(rig)), -32602);
        EXPECT_TRUE(rig.all<neomcp::ServerAsked>().empty());
    }
}

TEST(ClientInputRefusal, ARoundThatAsksForAnUndeclaredKindEndsTheCallWithoutARetry) {
    neomcp::InputHandlers handlers;
    handlers.form([](const neomcp::ElicitationForm&) { return accepting(json::object()); });
    Rig rig(opened(per_request_revision, handlers.support()));
    open(rig, per_request_revision);
    SyncRunner runner;
    Feed feed(rig, handlers, runner);
    runner.dispatcher = &feed.dispatcher;
    const auto ref = tool_call(rig);
    rig.answer("tools/call", asking_result(json{{"u", json{{"method", "elicitation/create"},
                                                           {"params", J(R"({"mode":"url","message":"Go",
                                                           "url":"https://example.com","elicitationId":"e1"})")}}}}));
    feed();
    const auto ended = rig.ended(ref);
    ASSERT_TRUE(ended);
    EXPECT_EQ(failure_of(*ended), CallFailureKind::server_asked_undeclared);
    EXPECT_EQ(runner.started, 0u);
    EXPECT_EQ(rig.requests().size(), 1u);
}

TEST(ClientInputRefusal, SamplingWithToolUseBlocksIsRefusedByTheReaderUnderBothMechanisms) {
    neomcp::InputHandlers handlers;
    handlers.sampling([](const neomcp::SamplingRequest&) { return completion("m"); }, neomcp::SamplingSupport{true, true});
    const json tool_use = J(R"({"messages":[{"role":"assistant","content":[{"type":"tool_use","id":"c1",
        "name":"lookup","input":{}}]}],"maxTokens":5})");
    {
        SCOPED_TRACE("server_request");
        Rig rig(opened(handshake_revision, handlers.support()));
        open(rig, handshake_revision);
        SyncRunner runner;
        Feed feed(rig, handlers, runner);
        runner.dispatcher = &feed.dispatcher;
        server_request(rig, 1, "sampling/createMessage", tool_use);
        feed();
        EXPECT_EQ(error_code(last_response(rig)), -32602);
        EXPECT_EQ(runner.started, 0u);
    }
    {
        SCOPED_TRACE("input_required_result");
        Rig rig(opened(per_request_revision, handlers.support()));
        open(rig, per_request_revision);
        SyncRunner runner;
        Feed feed(rig, handlers, runner);
        runner.dispatcher = &feed.dispatcher;
        const auto ref = tool_call(rig);
        rig.answer("tools/call", asking_result(json{{"s", json{{"method", "sampling/createMessage"}, {"params", tool_use}}}}));
        feed();
        const auto ended = rig.ended(ref);
        ASSERT_TRUE(ended);
        EXPECT_EQ(failure_of(*ended), CallFailureKind::malformed_answer);
        EXPECT_EQ(runner.started, 0u);
    }
}

bool refused(const neomcp::ClientInputMechanism& half, const json& schema, const neomcp::profile::Profile& profile) {
    return half.form_problem(schema, profile).has_value();
}

TEST(ClientInputForm, TheCheckFollowsTheRevisionsPrimitiveSchemaDefinitions) {
    // 2025-06-18 gives a string no default; 2025-11-25 does.
    const char* with_default = R"({"type":"object","properties":{"n":{"type":"string","minLength":1,"default":"x"}}})";
    for (const auto revision : {std::string_view("2025-06-18"), handshake_revision}) {
        SCOPED_TRACE(std::string(revision));
        const auto profile = profile_of(revision);
        const auto half = neomcp::builtin_client_mechanisms().make_input(
            profile->mechanism(neomcp::profile::Slot::server_input));
        const bool string_default = profile->type("StringSchema")->has_property("default");
        EXPECT_EQ(half->form_problem(J(with_default), *profile).has_value(), !string_default);
        EXPECT_FALSE(refused(*half, J(R"({"type":"object","properties":{"b":{"type":"boolean"}},
            "required":["b"],"$schema":"https://json-schema.org/draft/2020-12/schema"})"), *profile));
        EXPECT_FALSE(refused(*half, J(R"({"type":"object","properties":{"c":{"type":"string","enum":["a","b"]}}})"),
                                        *profile));
        // Members of the root other than its type and properties are not the form's restriction: a title and a
        // description as a schema generator writes them, and keywords the content's validation applies.
        EXPECT_FALSE(refused(*half, J(R"({"type":"object","title":"Contact","properties":{
            "username":{"type":"string","title":"Username","description":"User's response"}},
            "required":["username"]})"), *profile));
        EXPECT_FALSE(refused(*half, J(R"({"type":"object","properties":{},"additionalProperties":false})"), *profile));
        // A property of no definition, and no properties at all.
        EXPECT_TRUE(refused(*half, J(R"({"type":"object","properties":{"o":{"type":"object","properties":{}}}})"),
                                       *profile));
        EXPECT_TRUE(refused(*half, J(R"({"type":"object","properties":{"s":{"type":"string","pattern":"a"}}})"),
                                       *profile));
        EXPECT_TRUE(refused(*half, J(R"({"type":"object"})"), *profile));
        EXPECT_TRUE(refused(*half, J(R"({"properties":{}})"), *profile));
        EXPECT_TRUE(refused(*half, J(R"("a string")"), *profile));
    }
}

TEST(ClientInputForm, ASchemaThatDoesNotCompileWithinThePeerLimitsIsOutsideTheForm) {
    neomcp::InputHandlers handlers;
    handlers.form([](const neomcp::ElicitationForm&) { return accepting(J(R"({"n":"x"})")); });
    auto options = opened(handshake_revision, handlers.support());
    Rig rig(options);
    open(rig, handshake_revision);
    // `minLength` of the wrong kind fits the member names but does not compile.
    server_request(rig, 1, "elicitation/create",
                   form_params(R"({"type":"object","properties":{"n":{"type":"string","minLength":"long"}}})"));
    EXPECT_EQ(error_code(last_response(rig)), -32602);
    EXPECT_TRUE(rig.all<neomcp::ServerAsked>().empty());
}

TEST(ClientInputForm, PrefilledHoldsTheDefaultsOfTheProperties) {
    neomcp::ElicitationForm form;
    form.requested_schema = J(R"({"type":"object","properties":{"a":{"type":"string","default":"x"},
        "b":{"type":"boolean"},"c":{"type":"number","default":2}}})");
    EXPECT_EQ(form.prefilled(), J(R"({"a":"x","c":2})"));
    form.requested_schema = J(R"({"type":"object","properties":{"b":{"type":"boolean"}}})");
    EXPECT_EQ(form.prefilled(), json::object());
    form.requested_schema = J(R"([1,2])");
    EXPECT_EQ(form.prefilled(), json::object());
    form.requested_schema = J(R"({"properties":{"a":"not a schema","b":{"default":null}}})");
    EXPECT_EQ(form.prefilled(), J(R"({"b":null})"));
}

// ---------------------------------------------------------------------------------------------------------------
// Clocks and cancellation

TEST(ClientInputClock, AHandlerThatNeverAnswersIsStoppedWhenTheInputClockLapses) {
    neomcp::InputHandlers handlers;
    handlers.form([](const neomcp::ElicitationForm&) { return accepting(J(R"({"ok":true})")); });
    {
        SCOPED_TRACE("server_request");
        Rig rig(opened(handshake_revision, handlers.support()));
        open(rig, handshake_revision);
        DeferredRunner runner;
        Feed feed(rig, handlers, runner);
        runner.dispatcher = &feed.dispatcher;
        server_request(rig, 1, "elicitation/create", form_params(R"({"type":"object","properties":{}})"));
        feed();
        ASSERT_EQ(runner.jobs.size(), 1u);
        const auto asked = rig.all<neomcp::ServerAsked>().at(0);
        const auto clock = rig.clock(asked.ref, DeadlineKind::input);
        ASSERT_TRUE(clock);
        rig.engine->expire(clock->deadline);
        feed();
        const json cancelled = last_response(rig);
        ASSERT_TRUE(cancelled.contains("result"));
        EXPECT_EQ(cancelled["result"]["action"], "cancel");
        EXPECT_TRUE(runner.jobs[0].stop.stop_requested());
        EXPECT_EQ(feed.dispatcher.outstanding(), 0u);
        // What the handler returns afterwards is not sent.
        const auto written = rig.outputs(OutgoingKind::response).size();
        runner.run(0);
        feed();
        EXPECT_EQ(rig.outputs(OutgoingKind::response).size(), written);
    }
    {
        SCOPED_TRACE("input_required_result");
        Rig rig(opened(per_request_revision, handlers.support()));
        open(rig, per_request_revision);
        DeferredRunner runner;
        Feed feed(rig, handlers, runner);
        runner.dispatcher = &feed.dispatcher;
        const auto ref = tool_call(rig);
        rig.answer("tools/call", asking_result(json{{"f", json{{"method", "elicitation/create"},
                                                               {"params", form_params(R"({"type":"object",
                                                               "properties":{}})")}}}}));
        feed();
        ASSERT_EQ(runner.jobs.size(), 1u);
        const auto clock = rig.clock(ref, DeadlineKind::input);
        ASSERT_TRUE(clock);
        rig.engine->expire(clock->deadline);
        feed();
        const auto ended = rig.ended(ref);
        ASSERT_TRUE(ended);
        EXPECT_EQ(failure_of(*ended), CallFailureKind::input_timed_out);
        EXPECT_TRUE(runner.jobs[0].stop.stop_requested());
        EXPECT_EQ(feed.dispatcher.outstanding(), 0u);
        runner.run(0);
        feed();
        EXPECT_EQ(rig.requests().size(), 1u);
    }
}

TEST(ClientInputClock, TheServersCancellationStopsTheHandlerAndNothingIsAnswered) {
    neomcp::InputHandlers handlers;
    std::optional<std::stop_token> seen;
    handlers.form([&](const neomcp::ElicitationForm&, InputContext& context) {
        seen = context.stop_token();
        return accepting(json::object());
    });
    Rig rig(opened(handshake_revision, handlers.support()));
    open(rig, handshake_revision);
    DeferredRunner runner;
    Feed feed(rig, handlers, runner);
    runner.dispatcher = &feed.dispatcher;
    server_request(rig, 9, "elicitation/create", form_params(R"({"type":"object","properties":{}})"));
    feed();
    ASSERT_EQ(runner.jobs.size(), 1u);
    rig.engine->received(neomcp::connection_unit,
                         R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":9}})");
    feed();
    EXPECT_TRUE(runner.jobs[0].stop.stop_requested());
    EXPECT_EQ(feed.dispatcher.outstanding(), 0u);
    runner.run(0);
    feed();
    ASSERT_TRUE(seen);
    EXPECT_TRUE(seen->stop_requested());
    EXPECT_TRUE(rig.outputs(OutgoingKind::response).empty());
}

// ---------------------------------------------------------------------------------------------------------------
// The dispatcher's bookkeeping

TEST(ClientInputDispatcher, ItRefusesHandlersOtherThanThoseTheEngineDeclares) {
    neomcp::InputHandlers handlers;
    handlers.form([](const neomcp::ElicitationForm&) { return accepting(json::object()); });
    Rig rig(opened(per_request_revision, neomcp::InputSupport{}));
    SyncRunner runner;
    const auto make = [&] { neomcp::InputDispatcher dispatcher(*rig.engine, handlers, runner); };
    EXPECT_THROW(make(), std::invalid_argument);
}

TEST(ClientInputDispatcher, TheSupportFollowsTheHandlers) {
    neomcp::InputHandlers handlers;
    EXPECT_TRUE(handlers.empty());
    EXPECT_EQ(handlers.support(), neomcp::InputSupport{});
    handlers.sampling([](const neomcp::SamplingRequest&) { return completion("m"); }, neomcp::SamplingSupport{true, false})
        .roots([] { return work_root(); }, true);
    neomcp::InputSupport expected;
    expected.sampling = true;
    expected.sampling_tools = true;
    expected.roots = true;
    expected.roots_list_changed = true;
    EXPECT_EQ(handlers.support(), expected);
    EXPECT_FALSE(handlers.empty());
}

TEST(ClientInputDispatcher, ARoundAnswersOnceEveryHandlerAnsweredUnderTheKeysItAsked) {
    neomcp::InputHandlers handlers;
    handlers.form([](const neomcp::ElicitationForm& form) {
        return accepting(json{{"n", form.message}});
    });
    Rig rig(opened(per_request_revision, handlers.support()));
    open(rig, per_request_revision);
    DeferredRunner runner;
    Feed feed(rig, handlers, runner);
    runner.dispatcher = &feed.dispatcher;
    const auto ref = tool_call(rig);
    const char* schema = R"({"type":"object","properties":{"n":{"type":"string"}}})";
    json first = json{{"method", "elicitation/create"}, {"params", form_params(schema)}};
    first["params"]["message"] = "one";
    json second = first;
    second["params"]["message"] = "two";
    rig.answer("tools/call", asking_result(json{{"a", first}, {"b", second}}));
    feed();
    ASSERT_EQ(runner.jobs.size(), 2u);
    EXPECT_EQ(feed.dispatcher.outstanding(), 2u);
    runner.run(1);
    feed();
    EXPECT_EQ(rig.requests().size(), 1u);
    EXPECT_EQ(feed.dispatcher.outstanding(), 1u);
    // A job completed twice counts once.
    runner.run(1);
    feed();
    EXPECT_EQ(rig.requests().size(), 1u);
    runner.run(0);
    feed();
    ASSERT_EQ(rig.requests().size(), 2u);
    const json retry = rig.requests().back();
    ASSERT_TRUE(retry["params"].contains("inputResponses"));
    ASSERT_TRUE(retry["params"]["inputResponses"].contains("a"));
    ASSERT_TRUE(retry["params"]["inputResponses"].contains("b"));
    EXPECT_EQ(retry["params"]["inputResponses"]["a"]["content"]["n"], "one");
    EXPECT_EQ(retry["params"]["inputResponses"]["b"]["content"]["n"], "two");
    EXPECT_EQ(retry["params"]["requestState"], "st");
    EXPECT_EQ(feed.dispatcher.outstanding(), 0u);
    EXPECT_FALSE(rig.ended(ref));
}

TEST(ClientInputDispatcher, ARoundThatAsksForNothingButItsStateGoesOutAgainWithTheStateAlone) {
    neomcp::InputHandlers handlers;
    handlers.form([](const neomcp::ElicitationForm&) { return accepting(json::object()); });
    Rig rig(opened(per_request_revision, handlers.support()));
    open(rig, per_request_revision);
    SyncRunner runner;
    Feed feed(rig, handlers, runner);
    runner.dispatcher = &feed.dispatcher;
    const auto ref = tool_call(rig);
    rig.answer("tools/call", J(R"({"resultType":"input_required","requestState":"only"})"));
    feed();
    EXPECT_EQ(runner.started, 0u);
    ASSERT_EQ(rig.requests().size(), 2u);
    const json retry = rig.requests().back();
    EXPECT_EQ(retry["params"]["requestState"], "only");
    EXPECT_FALSE(retry["params"].contains("inputResponses"));
    EXPECT_FALSE(rig.ended(ref));
}

TEST(ClientInputDispatcher, ACoroutineHandlerThatThrowsEndsItsRound) {
    neomcp::InputHandlers handlers;
    handlers.form([](neomcp::ElicitationForm form) -> asio::awaitable<ElicitationAnswer> {
        if (form.requested_schema.is_object()) throw std::runtime_error("coroutine-detail");
        co_return ElicitationAnswer{};
    });
    Rig rig(opened(per_request_revision, handlers.support()));
    open(rig, per_request_revision);
    SyncRunner runner;
    Feed feed(rig, handlers, runner);
    runner.dispatcher = &feed.dispatcher;
    const auto ref = tool_call(rig);
    const json form = json{{"method", "elicitation/create"}, {"params", form_params(R"({"type":"object","properties":{}})")}};
    rig.answer("tools/call", asking_result(json{{"a", form}}));
    feed();
    const auto ended = rig.ended(ref);
    ASSERT_TRUE(ended);
    EXPECT_EQ(failure_of(*ended), CallFailureKind::handler_failed);
    const auto reports = feed.reports();
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].message, "coroutine-detail");
    EXPECT_EQ(rig.requests().size(), 1u);
}

TEST(ClientInputDispatcher, AFailedHandlerEndsItsRoundAndStopsTheOthers) {
    neomcp::InputHandlers handlers;
    handlers.form([](const neomcp::ElicitationForm&) { return accepting(json::object()); });
    Rig rig(opened(per_request_revision, handlers.support()));
    open(rig, per_request_revision);
    DeferredRunner runner;
    Feed feed(rig, handlers, runner);
    runner.dispatcher = &feed.dispatcher;
    const auto ref = tool_call(rig);
    const json form = json{{"method", "elicitation/create"}, {"params", form_params(R"({"type":"object","properties":{}})")}};
    rig.answer("tools/call", asking_result(json{{"a", form}, {"b", form}, {"c", form}}));
    feed();
    ASSERT_EQ(runner.jobs.size(), 3u);
    runner.complete(1, neomcp::HandlerFailed{"broken"});
    feed();
    const auto ended = rig.ended(ref);
    ASSERT_TRUE(ended);
    EXPECT_EQ(failure_of(*ended), CallFailureKind::handler_failed);
    EXPECT_TRUE(runner.jobs[0].stop.stop_requested());
    EXPECT_TRUE(runner.jobs[2].stop.stop_requested());
    EXPECT_EQ(feed.dispatcher.outstanding(), 0u);
    const auto reports = feed.reports();
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].key, std::optional<std::string>("b"));
    EXPECT_EQ(reports[0].call, ref);
    runner.run(0);
    runner.run(2);
    feed();
    EXPECT_EQ(rig.requests().size(), 1u);
}

TEST(ClientInputDispatcher, ACancelledCallStopsTheHandlersOfItsRound) {
    neomcp::InputHandlers handlers;
    handlers.form([](const neomcp::ElicitationForm&) { return accepting(json::object()); });
    Rig rig(opened(per_request_revision, handlers.support()));
    open(rig, per_request_revision);
    DeferredRunner runner;
    Feed feed(rig, handlers, runner);
    runner.dispatcher = &feed.dispatcher;
    const auto ref = tool_call(rig);
    const json form = json{{"method", "elicitation/create"}, {"params", form_params(R"({"type":"object","properties":{}})")}};
    rig.answer("tools/call", asking_result(json{{"a", form}}));
    feed();
    ASSERT_EQ(runner.jobs.size(), 1u);
    rig.engine->cancel(ref);
    feed();
    EXPECT_TRUE(runner.jobs[0].stop.stop_requested());
    EXPECT_EQ(feed.dispatcher.outstanding(), 0u);
}

TEST(ClientInputDispatcher, CloseStopsEveryHandlerAndLaterOutcomesAreIgnored) {
    neomcp::InputHandlers handlers;
    handlers.form([](const neomcp::ElicitationForm&) { return accepting(json::object()); });
    Rig rig(opened(handshake_revision, handlers.support()));
    open(rig, handshake_revision);
    DeferredRunner runner;
    Feed feed(rig, handlers, runner);
    runner.dispatcher = &feed.dispatcher;
    server_request(rig, 1, "elicitation/create", form_params(R"({"type":"object","properties":{}})"));
    server_request(rig, 2, "elicitation/create", form_params(R"({"type":"object","properties":{}})"));
    feed();
    ASSERT_EQ(runner.jobs.size(), 2u);
    feed.dispatcher.close();
    EXPECT_TRUE(runner.jobs[0].stop.stop_requested());
    EXPECT_TRUE(runner.jobs[1].stop.stop_requested());
    EXPECT_EQ(feed.dispatcher.outstanding(), 0u);
    runner.run(0);
    feed();
    EXPECT_TRUE(rig.outputs(OutgoingKind::response).empty());
}

// ---------------------------------------------------------------------------------------------------------------
// Capabilities and roots

// Every combination of handlers.
std::vector<neomcp::InputSupport> every_support() {
    std::vector<neomcp::InputSupport> all;
    for (int bits = 0; bits < (1 << 7); ++bits) {
        neomcp::InputSupport support;
        support.form = (bits & 1) != 0;
        support.url = (bits & 2) != 0;
        support.sampling = (bits & 4) != 0;
        support.sampling_tools = support.sampling && (bits & 8) != 0;
        support.sampling_context = support.sampling && (bits & 16) != 0;
        support.roots = (bits & 32) != 0;
        support.roots_list_changed = support.roots && (bits & 64) != 0;
        if (((bits & 8) != 0 || (bits & 16) != 0) && !support.sampling) continue;
        if ((bits & 64) != 0 && !support.roots) continue;
        all.push_back(support);
    }
    return all;
}

TEST(ClientInputCapabilities, NothingIsWrittenForAKindWithoutAHandler) {
    for (const auto revision : neomcp::profile::builtin_revisions()) {
        const auto profile = profile_of(revision);
        const auto* type = profile->type("ClientCapabilities");
        for (const auto& support : every_support()) {
            const json written = neomcp::client_capabilities(support, *profile);
            SCOPED_TRACE(std::string(revision) + " " + written.dump());
            if (!support.form && !support.url) {
                EXPECT_FALSE(written.contains("elicitation"));
            }
            if (!support.sampling) {
                EXPECT_FALSE(written.contains("sampling"));
            }
            if (!support.roots) {
                EXPECT_FALSE(written.contains("roots"));
            }
            for (const auto& [name, unused] : written.get_ref<const json::object_t&>()) {
                static_cast<void>(unused);
                EXPECT_TRUE(type->has_property(name)) << name;
            }
            // What is written declares what the client takes, wherever the revision carries it.
            if (support.sampling) {
                EXPECT_TRUE(written.contains("sampling"));
            }
            if (support.roots) {
                EXPECT_TRUE(written.contains("roots"));
            }
            if (support.form && type->has_property("elicitation")) {
                EXPECT_TRUE(written.contains("elicitation"));
            }
        }
    }
}

// An object's members in name order, at every level.
json in_name_order(const json& value) {
    if (!value.is_object()) return value;
    std::map<std::string, json> members;
    for (const auto& [name, member] : value.get_ref<const json::object_t&>()) members.emplace(name, in_name_order(member));
    json ordered = json::object();
    for (auto& [name, member] : members) ordered[name] = std::move(member);
    return ordered;
}

TEST(ClientInputCapabilities, NestedMembersAreWrittenOnlyWhereTheRevisionCarriesThem) {
    neomcp::InputSupport everything;
    everything.form = everything.url = everything.sampling = everything.sampling_tools = true;
    everything.sampling_context = everything.roots = everything.roots_list_changed = true;
    neomcp::InputSupport url_only;
    url_only.url = true;
    neomcp::InputSupport form_only;
    form_only.form = true;
    // The members in name order, so that the comparison does not depend on the order they were written in.
    const auto write = [](const neomcp::InputSupport& support, std::string_view revision) {
        return in_name_order(neomcp::client_capabilities(support, *profile_of(revision)));
    };
    EXPECT_EQ(write(everything, "2024-11-05"), in_name_order(J(R"({"roots":{"listChanged":true},"sampling":{"context":{}}})")));
    EXPECT_EQ(write(everything, "2025-06-18"),
              in_name_order(J(R"({"elicitation":{},"roots":{"listChanged":true},"sampling":{"context":{}}})")));
    EXPECT_EQ(write(everything, "2025-11-25"), in_name_order(J(R"({"elicitation":{"form":{},"url":{}},"roots":{"listChanged":true},
        "sampling":{"context":{},"tools":{}}})")));
    EXPECT_EQ(write(everything, "2026-07-28"),
              in_name_order(J(R"({"elicitation":{"form":{},"url":{}},"roots":{},"sampling":{"context":{},"tools":{}}})")));
    EXPECT_EQ(write(url_only, "2025-06-18"), json::object());
    EXPECT_EQ(write(url_only, "2025-11-25"), in_name_order(J(R"({"elicitation":{"url":{}}})")));
    EXPECT_EQ(write(form_only, "2025-06-18"), in_name_order(J(R"({"elicitation":{}})")));
    EXPECT_EQ(write(form_only, "2025-11-25"), in_name_order(J(R"({"elicitation":{"form":{}}})")));
    EXPECT_EQ(write(neomcp::InputSupport{}, "2025-11-25"), json::object());
}

TEST(ClientInputCapabilities, TheEngineDeclaresWhatTheWriterWrites) {
    neomcp::InputHandlers handlers;
    handlers.form([](const neomcp::ElicitationForm&) { return accepting(json::object()); })
        .roots([] { return work_root(); }, true);
    {
        Rig rig(opened(handshake_revision, handlers.support()));
        open(rig, handshake_revision);
        EXPECT_EQ(rig.last("initialize")->first["params"]["capabilities"],
                  neomcp::client_capabilities(handlers.support(), *profile_of(handshake_revision)));
    }
    {
        Rig rig(opened(per_request_revision, handlers.support()));
        open(rig, per_request_revision);
        rig.send("tools/list", json::object());
        const json meta = rig.last("tools/list")->first["params"]["_meta"];
        EXPECT_EQ(meta["io.modelcontextprotocol/clientCapabilities"],
                  neomcp::client_capabilities(handlers.support(), *profile_of(per_request_revision)));
    }
}

TEST(ClientInputRoots, TheChangeIsToldOnlyWhereTheRevisionHasItAndTheClientDeclaredIt) {
    for (const bool notify : {false, true}) {
        neomcp::InputHandlers handlers;
        handlers.roots([] { return work_root(); }, notify);
        Rig rig(opened(handshake_revision, handlers.support()));
        EXPECT_FALSE(rig.engine->roots_changed());  // contact is not open yet
        open(rig, handshake_revision);
        EXPECT_EQ(rig.engine->roots_changed(), notify);
        bool told = false;
        for (const auto& outgoing : rig.outputs(OutgoingKind::notification)) {
            told = told || json::parse(outgoing.line)["method"] == "notifications/roots/list_changed";
        }
        EXPECT_EQ(told, notify);
    }
    neomcp::InputHandlers handlers;
    handlers.roots([] { return work_root(); }, true);
    Rig rig(opened(per_request_revision, handlers.support()));
    open(rig, per_request_revision);
    EXPECT_FALSE(rig.engine->roots_changed());
    EXPECT_TRUE(rig.outputs(OutgoingKind::notification).empty());
}

TEST(ClientInputEngine, ZeroBoundsAreRefusedAtCreation) {
    for (int which = 0; which < 5; ++which) {
        auto options = only({per_request_revision});
        switch (which) {
            case 0: options.max_input_requests_per_round = 0; break;
            case 1: options.peer_schema_depth = 0; break;
            case 2: options.peer_pattern_length = 0; break;
            case 3: options.peer_pattern_memory = 0; break;
            default: options.peer_max_evaluations = 0; break;
        }
        auto made = neomcp::ClientEngine::create(options, Transport::stdio, [](neomcp::Outgoing) {});
        EXPECT_TRUE(std::holds_alternative<neomcp::ClientMechanismsError>(made)) << which;
    }
}

}  // namespace
