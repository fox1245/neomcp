// The bounds on what a server controls that the engine and the Client keep, each driven to its edge with its default:
// at the bound the server is served, one past it the documented outcome follows, and the client goes on where it can.
// Clocks are the engine's deadlines, fired by the test.
#include <gtest/gtest.h>

#include <cstddef>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

#include "../client_core/engine_rig.h"
#include "support.h"

namespace {

namespace et = client_core_test;
namespace ct = client_test;
using neomcp::CallFailureKind;
using neomcp::ClientDefaults;
using neomcp::ContactPhase;
using neomcp::DeadlineAction;
using neomcp::DeadlineKind;
using neomcp::OutgoingKind;
using neomcp::RequestRef;
using neomcp::json;
using neomcp::profile::Transport;

constexpr std::string_view newest = "2026-07-28";
constexpr std::string_view handshake_revision = "2025-11-25";
constexpr std::string_view batching_revision = "2025-03-26";

neomcp::EngineOptions with_prior(neomcp::EngineOptions options, Transport transport, std::string_view revision) {
    options.prior = neomcp::ContactVerdict{neomcp::ServerKind::per_request, transport, std::string(revision)};
    return options;
}

neomcp::OutgoingCall tool_call() {
    neomcp::OutgoingCall call;
    call.message.method = "tools/call";
    call.message.params = json{{"name", "delete"}, {"arguments", json::object()}};
    return call;
}

neomcp::OutgoingCall list_call() {
    neomcp::OutgoingCall call;
    call.message.method = "tools/list";
    call.message.params = json::object();
    return call;
}

CallFailureKind failure_of(const std::optional<neomcp::RequestEnded>& ended) {
    if (!ended) throw std::runtime_error("the call did not end");
    const auto* failure = std::get_if<neomcp::CallFailure>(&ended->outcome);
    if (failure == nullptr) throw std::runtime_error("the call did not fail");
    return failure->kind;
}

std::size_t count_requests(const et::Rig& rig, std::string_view method) {
    std::size_t count = 0;
    for (const auto& request : rig.requests()) count += request["method"] == std::string(method) ? 1 : 0;
    return count;
}

json last_response(const et::Rig& rig) {
    for (auto it = rig.out.rbegin(); it != rig.out.rend(); ++it) {
        if (it->kind == OutgoingKind::response) return json::parse(it->line);
    }
    throw std::runtime_error("no response was written");
}

json form_request() {
    const json schema = json::parse(R"({"type":"object","properties":{"ok":{"type":"boolean"}}})");
    return json{{"method", "elicitation/create"},
                {"params", json{{"mode", "form"}, {"message", "Sure?"}, {"requestedSchema", schema}}}};
}

// A result that asks for `forms` forms in one round, under the keys k0, k1, ...
json asking_result(std::size_t forms, std::string_view state) {
    json requests = json::object();
    for (std::size_t i = 0; i < forms; ++i) requests["k" + std::to_string(i)] = form_request();
    return json{{"resultType", "input_required"}, {"inputRequests", requests}, {"requestState", std::string(state)}};
}

void open_handshake(et::Rig& rig) {
    rig.engine->open();
    rig.answer("initialize", et::initialize_result(handshake_revision));
    ASSERT_EQ(rig.engine->contact().phase, ContactPhase::open);
}

std::string server_request(const std::string& id, std::string_view method, const json& params = nullptr) {
    json message{{"jsonrpc", "2.0"}, {"id", id}, {"method", std::string(method)}};
    if (!params.is_null()) message["params"] = params;
    return message.dump();
}

// ---- Calls pending

TEST(ClientBounds, PendingCallsAreTakenUpToTheBoundAndOneMoreFailsAtOnce) {
    constexpr std::size_t bound = ClientDefaults::max_pending_requests;
    et::Rig rig(with_prior(et::only({newest}), Transport::stdio, newest));
    rig.engine->open();
    for (std::size_t i = 0; i < bound; ++i) rig.send("tools/list");
    EXPECT_EQ(rig.engine->pending(), bound);
    EXPECT_EQ(count_requests(rig, "tools/list"), bound);
    const auto refused = rig.engine->send(list_call());
    ASSERT_TRUE(std::holds_alternative<neomcp::CallFailure>(refused));
    EXPECT_EQ(std::get<neomcp::CallFailure>(refused).kind, CallFailureKind::too_many_pending);
    EXPECT_EQ(count_requests(rig, "tools/list"), bound);  // nothing went out for it
    // One call ends: there is room for one more, and no more.
    rig.answer("tools/list", json{{"tools", json::array()}});
    EXPECT_EQ(rig.engine->pending(), bound - 1);
    rig.send("tools/list");
    EXPECT_EQ(rig.engine->pending(), bound);
    EXPECT_TRUE(std::holds_alternative<neomcp::CallFailure>(rig.engine->send(list_call())));
}

// ---- Input that comes in results

TEST(ClientBounds, ARoundThatAsksForTheBoundOfInputsReachesTheHandlersAndOneMoreEndsTheCall) {
    constexpr std::size_t bound = ClientDefaults::max_input_requests_per_round;
    auto options = with_prior(et::only({newest}), Transport::stdio, newest);
    options.input.form = true;
    et::Rig rig(options);
    rig.engine->open();
    const auto at = rig.send(tool_call());
    rig.answer("tools/call", asking_result(bound, "at"));
    const auto rounds = rig.all<neomcp::InputRoundStarted>();
    ASSERT_EQ(rounds.size(), 1u);
    EXPECT_EQ(rounds[0].request, at);
    EXPECT_EQ(rounds[0].asking.requests.size(), bound);
    EXPECT_FALSE(rig.ended(at));

    const auto past = rig.send(tool_call());
    rig.answer("tools/call", asking_result(bound + 1, "past"));
    EXPECT_EQ(failure_of(rig.ended(past)), CallFailureKind::input_requests_exceeded);
    EXPECT_EQ(rig.all<neomcp::InputRoundStarted>().size(), 1u);  // no handler sees any of them
    EXPECT_EQ(count_requests(rig, "tools/call"), 2u);              // and nothing is sent again

    // The call within the bound goes on.
    neomcp::InputResponses answers;
    for (std::size_t i = 0; i < bound; ++i) {
        answers.emplace("k" + std::to_string(i),
                        neomcp::ElicitationAnswer{neomcp::ElicitationAction::decline, json::object()});
    }
    rig.engine->answer_round(at, answers);
    EXPECT_EQ(count_requests(rig, "tools/call"), 3u);
    rig.answer("tools/call", json{{"content", json::array()}, {"resultType", "complete"}});
    EXPECT_TRUE(std::holds_alternative<neomcp::Answered>(rig.ended(at)->outcome));
}

TEST(ClientBounds, TheBoundOfRoundsIsServedAndOneRoundMoreEndsTheCall) {
    constexpr std::size_t bound = ClientDefaults::max_input_rounds;
    auto options = with_prior(et::only({newest}), Transport::stdio, newest);
    options.input.form = true;
    et::Rig rig(options);
    rig.engine->open();
    const auto ref = rig.send(tool_call());
    neomcp::InputResponses answers;
    answers.emplace("k0", neomcp::ElicitationAnswer{neomcp::ElicitationAction::decline, json::object()});
    for (std::size_t round = 1; round <= bound; ++round) {
        rig.answer("tools/call", asking_result(1, "s" + std::to_string(round)));
        const auto rounds = rig.all<neomcp::InputRoundStarted>();
        ASSERT_EQ(rounds.size(), round);
        EXPECT_EQ(rounds.back().round, round);
        rig.engine->answer_round(ref, answers);
    }
    EXPECT_EQ(count_requests(rig, "tools/call"), bound + 1);
    rig.answer("tools/call", asking_result(1, "once more"));
    EXPECT_EQ(failure_of(rig.ended(ref)), CallFailureKind::input_rounds_exceeded);
    EXPECT_EQ(rig.all<neomcp::InputRoundStarted>().size(), bound);
    EXPECT_EQ(count_requests(rig, "tools/call"), bound + 1);

    // The engine goes on: another call is answered.
    const auto other = rig.send(tool_call());
    rig.answer("tools/call", json{{"content", json::array()}, {"resultType", "complete"}});
    EXPECT_TRUE(std::holds_alternative<neomcp::Answered>(rig.ended(other)->outcome));
}

// ---- Requests of a server (revisions whose servers send them)

// On a call's own exchange the wire says which call a request belongs to: the bound of rounds counts them, and past it
// a request is refused and the call's clock no longer waits for the client.
TEST(ClientBounds, RequestsTiedToACallPauseItsClockUpToTheBoundOfRounds) {
    constexpr std::size_t bound = ClientDefaults::max_input_rounds;
    auto options = et::only({handshake_revision});
    options.input.roots = true;
    et::Rig rig(options, Transport::streamable_http);
    open_handshake(rig);
    const auto call = rig.send(tool_call());
    const auto unit = rig.last_unit("tools/call");
    for (std::size_t i = 0; i < bound; ++i) {
        rig.engine->received(unit, server_request("r" + std::to_string(i), "roots/list"));
        const auto asked = rig.all<neomcp::ServerAsked>();
        ASSERT_EQ(asked.size(), i + 1);
        ASSERT_TRUE(asked.back().call.has_value());
        EXPECT_EQ(*asked.back().call, call);
        EXPECT_EQ(rig.clock(call, DeadlineKind::wait)->action, DeadlineAction::pause);
        rig.engine->answer(asked.back().ref, neomcp::InputResponse(neomcp::RootsAnswer{}));
        EXPECT_EQ(rig.clock(call, DeadlineKind::wait)->action, DeadlineAction::resume);
    }
    const std::size_t changes = rig.all<neomcp::DeadlineChange>().size();
    rig.engine->received(unit, server_request("past", "roots/list"));
    const json busy = last_response(rig);
    EXPECT_EQ(busy["id"], "past");
    EXPECT_EQ(busy["error"]["code"], -32603);
    EXPECT_EQ(rig.all<neomcp::ServerAsked>().size(), bound);  // no handler sees it
    EXPECT_EQ(rig.all<neomcp::DeadlineChange>().size(), changes);  // and no clock waits for it
    EXPECT_FALSE(rig.ended(call));
    // The call's answer still ends it.
    rig.answer("tools/call", json{{"content", json::array()}});
    EXPECT_TRUE(std::holds_alternative<neomcp::Answered>(rig.ended(call)->outcome));
}

TEST(ClientBounds, ServerRequestsUpToTheBoundAreTakenAndOneMoreIsRefusedUntilOneEnds) {
    constexpr std::size_t bound = ClientDefaults::max_concurrent_server_requests;
    auto options = et::only({handshake_revision});
    options.input.roots = true;
    et::Rig rig(options);
    open_handshake(rig);
    for (std::size_t i = 0; i < bound; ++i) {
        rig.engine->received(neomcp::connection_unit, server_request("r" + std::to_string(i), "roots/list"));
    }
    const auto asked = rig.all<neomcp::ServerAsked>();
    ASSERT_EQ(asked.size(), bound);
    EXPECT_TRUE(rig.outputs(OutgoingKind::response).empty());
    // A ping does not count, and is answered at the bound too.
    rig.engine->received(neomcp::connection_unit, server_request("ping", "ping"));
    EXPECT_EQ(last_response(rig)["result"], json::object());
    rig.engine->received(neomcp::connection_unit, server_request("past", "roots/list"));
    const json busy = last_response(rig);
    EXPECT_EQ(busy["id"], "past");
    EXPECT_EQ(busy["error"]["code"], -32603);
    EXPECT_EQ(rig.all<neomcp::ServerAsked>().size(), bound);
    // One is answered: the next request is taken again.
    rig.engine->answer(asked.front().ref, neomcp::InputResponse(neomcp::RootsAnswer{}));
    rig.engine->received(neomcp::connection_unit, server_request("again", "roots/list"));
    EXPECT_EQ(rig.all<neomcp::ServerAsked>().size(), bound + 1);
    EXPECT_EQ(last_response(rig)["id"], "r0");
}

// The server's question holds the clocks of the calls it pauses for input_handler_timeout at most; then a form or a
// URL request is answered as dismissed, any other kind with an error, and the calls wait on.
TEST(ClientBounds, AnUnansweredQuestionIsAnsweredWhenTheInputClockLapses) {
    auto options = et::only({handshake_revision});
    options.input.form = true;
    options.input.url = true;
    options.input.roots = true;
    et::Rig rig(options);
    open_handshake(rig);
    const auto call = rig.send("tools/list");
    const json form = form_request()["params"];
    const json url{{"mode", "url"}, {"message", "Sign in"}, {"url", "https://example.com/sign-in"},
                   {"elicitationId", "e1"}};
    struct Case {
        std::string id;
        std::string method;
        json params;
        bool dismissed;
    };
    const std::vector<Case> cases{{"form", "elicitation/create", form, true},
                                  {"url", "elicitation/create", url, true},
                                  {"roots", "roots/list", nullptr, false}};
    for (const Case& one : cases) {
        SCOPED_TRACE(one.id);
        rig.engine->received(neomcp::connection_unit, server_request(one.id, one.method, one.params));
        const auto asked = rig.all<neomcp::ServerAsked>().back();
        const auto clock = rig.clock(asked.ref, DeadlineKind::input);
        ASSERT_TRUE(clock.has_value());
        EXPECT_EQ(clock->action, DeadlineAction::start);
        EXPECT_EQ(clock->duration, ClientDefaults::input_handler_timeout);
        EXPECT_EQ(rig.clock(call, DeadlineKind::wait)->action, DeadlineAction::pause);
        rig.engine->expire(clock->deadline);
        const json answer = last_response(rig);
        EXPECT_EQ(answer["id"], one.id);
        if (one.dismissed) {
            EXPECT_EQ(answer["result"]["action"], "cancel");
        } else {
            EXPECT_EQ(answer["error"]["code"], -32603);
        }
        EXPECT_EQ(rig.clock(call, DeadlineKind::wait)->action, DeadlineAction::resume);
    }
    EXPECT_EQ(rig.all<neomcp::ServerAskEnded>().size(), cases.size());
    EXPECT_FALSE(rig.ended(call));
    rig.answer("tools/list", json{{"tools", json::array()}});
    EXPECT_TRUE(rig.ended(call));
}

// ---- The budget of a server's schemas

TEST(ClientBounds, TheCompileBudgetIsSpentAtItsBoundAndNotGivenBackByARestart) {
    constexpr std::size_t bound = ClientDefaults::max_peer_schema_compiles;
    et::Rig rig(with_prior(et::only({newest}), Transport::stdio, newest));
    rig.engine->open();
    const json schema = json::parse(R"({"type":"object"})");
    for (std::size_t i = 0; i + 1 < bound; ++i) {
        ASSERT_TRUE(std::holds_alternative<neomcp::json_schema::Validator>(rig.engine->compile_peer_schema(schema)));
    }
    // The server process ends and a new one runs.
    rig.send("tools/list");
    rig.engine->stream_event(neomcp::connection_unit, neomcp::ProcessExited{1});
    ASSERT_EQ(rig.engine->contact().phase, ContactPhase::lost);
    rig.send("tools/list");
    rig.engine->transport_restarted();
    ASSERT_EQ(rig.engine->contact().phase, ContactPhase::open);
    // The last compile of the budget, then none.
    EXPECT_TRUE(std::holds_alternative<neomcp::json_schema::Validator>(rig.engine->compile_peer_schema(schema)));
    EXPECT_EQ(rig.engine->peer_schema_compiles(), bound);
    const auto refused = rig.engine->compile_peer_schema(schema);
    ASSERT_TRUE(std::holds_alternative<neomcp::PeerSchemaRefused>(refused));
    EXPECT_TRUE(std::get<neomcp::PeerSchemaRefused>(refused).budget_spent);
    EXPECT_TRUE(std::get<neomcp::PeerSchemaRefused>(rig.engine->compile_peer_schema(schema)).budget_spent);
    EXPECT_EQ(rig.engine->peer_schema_compiles(), bound);
    EXPECT_EQ(rig.diagnostics(neomcp::DiagnosticKind::schema_budget_spent).size(), 1u);
}

// ---- Events queued for the application

// The client takes the engine's events after every message, so its queue fills only with what one message brings: a
// batch, under a revision that reads them. Within the bound every log message reaches the application; past it the
// rest are dropped, the application is told once, and the answer of the batch is never dropped.
TEST(ClientBounds, LogMessagesPastTheQueueAreDroppedAndToldAndTheAnswerIsKept) {
    constexpr std::size_t bound = ClientDefaults::max_queued_events;
    auto peer = std::make_shared<ct::ScriptedPeer>([](ct::ScriptedPeer& side, const ct::Seen& request) {
        if (request.method() == "initialize") {
            side.answer(request, ct::initialize_result(batching_revision));
            return;
        }
        if (request.method() != "tools/call") return;
        const std::size_t logs = request.message["params"]["arguments"]["logs"].get<std::size_t>();
        json batch = json::array();
        for (std::size_t i = 0; i < logs; ++i) {
            batch.push_back(json{{"jsonrpc", "2.0"},
                                 {"method", "notifications/message"},
                                 {"params", json{{"level", "info"}, {"data", i}}}});
        }
        batch.push_back(neomcp::make_result(neomcp::RequestId(request.message["id"].get<std::uint64_t>()),
                                            json{{"content", json::array()}}));
        side.send(request.unit, batch.dump());
    });
    std::mutex mutex;
    std::size_t logs = 0;
    std::size_t told = 0;
    auto options = ct::only({batching_revision});
    options.on_log = [&](const neomcp::LogEvent&) {
        std::lock_guard lock(mutex);
        ++logs;
    };
    options.on_diagnostic = [&](const neomcp::ClientDiagnostic& diagnostic) {
        const auto* engine = std::get_if<neomcp::Diagnostic>(&diagnostic);
        if (engine == nullptr || engine->kind != neomcp::DiagnosticKind::events_dropped) return;
        std::lock_guard lock(mutex);
        ++told;
    };
    auto opened = neomcp::Client::open(std::move(options), neomcp::make_memory_transport(Transport::stdio, peer));
    ASSERT_TRUE(opened) << opened.error().message;
    neomcp::Client client = std::move(opened).value();

    const auto at = client.call_tool("log", json{{"logs", bound}});
    ASSERT_TRUE(at) << at.error().message;
    {
        std::lock_guard lock(mutex);
        EXPECT_EQ(logs, bound);
        EXPECT_EQ(told, 0u);
        logs = 0;
    }
    const auto past = client.call_tool("log", json{{"logs", bound + 1}});
    ASSERT_TRUE(past) << past.error().message;
    {
        std::lock_guard lock(mutex);
        EXPECT_EQ(logs, bound);
        EXPECT_EQ(told, 1u);
        logs = 0;
    }
    // With room again, nothing is dropped and nothing more is told.
    const auto again = client.call_tool("log", json{{"logs", 1}});
    ASSERT_TRUE(again) << again.error().message;
    std::lock_guard lock(mutex);
    EXPECT_EQ(logs, 1u);
    EXPECT_EQ(told, 1u);
}

// ---- The defaults

// Every bound on what a server controls, with the default it is documented with, in the structs the defaults live in
// and in the options that take them.
TEST(ClientBounds, TheDefaultsAreTheDocumentedOnes) {
    using std::chrono::milliseconds;
    using D = ClientDefaults;
    EXPECT_EQ(D::max_message_bytes, std::size_t{16} * 1024 * 1024);
    EXPECT_EQ(D::max_nesting_depth, 128u);
    EXPECT_EQ(D::max_queued_events, 1024u);
    EXPECT_EQ(D::max_list_pages, 64u);
    EXPECT_EQ(D::max_list_items, 10'000u);
    EXPECT_EQ(neomcp::default_header_schema_depth, 64u);
    EXPECT_EQ(D::peer_schema_depth, 32u);
    EXPECT_EQ(D::peer_pattern_length, 256u);
    EXPECT_EQ(D::peer_pattern_memory, std::size_t{512} * 1024);
    EXPECT_EQ(D::peer_patterns_per_schema, 32u);
    EXPECT_EQ(D::max_peer_schema_compiles, 128u);
    EXPECT_EQ(D::peer_max_evaluations, 1'000'000u);
    EXPECT_EQ(D::max_input_rounds, 10u);
    EXPECT_EQ(D::input_handler_timeout, milliseconds(600'000));
    EXPECT_EQ(D::max_input_requests_per_round, 16u);
    EXPECT_EQ(D::max_concurrent_server_requests, 16u);
    EXPECT_EQ(D::max_pending_requests, 256u);
    EXPECT_EQ(D::max_subscriptions, 16u);
    EXPECT_EQ(D::discover_probe_timeout, milliseconds(10'000));
    EXPECT_EQ(D::max_restarts, 3u);
    EXPECT_EQ(D::max_subscription_reopens, 5u);

    using P = neomcp::ChildProcessDefaults;
    EXPECT_EQ(P::max_ignored_stdout_lines, 100u);
    EXPECT_EQ(P::max_ignored_stdout_bytes, std::size_t{64} * 1024);
    EXPECT_EQ(P::stderr_tail_bytes, std::size_t{64} * 1024);
    EXPECT_EQ(P::max_stderr_line_bytes, std::size_t{4} * 1024);
    EXPECT_EQ(P::exit_grace, milliseconds(2'000));
    EXPECT_EQ(P::term_grace, milliseconds(2'000));

    using H = neomcp::HttpClientDefaults;
    EXPECT_EQ(H::max_response_header_bytes, std::size_t{64} * 1024);
    EXPECT_EQ(H::max_session_id_bytes, 1024u);
    EXPECT_EQ(H::stream_idle_timeout, milliseconds(300'000));
    EXPECT_EQ(H::max_retry_delay, milliseconds(60'000));
    EXPECT_EQ(H::max_resumptions, 3u);
    EXPECT_EQ(H::max_redirects, 5u);
    EXPECT_EQ(H::endpoint_timeout, milliseconds(10'000));
    EXPECT_EQ(H::connect_timeout, milliseconds(10'000));

    // The options take them.
    const neomcp::ClientOptions client;
    const neomcp::EngineOptions engine;
    EXPECT_EQ(client.max_message_bytes, D::max_message_bytes);
    EXPECT_EQ(engine.max_message_bytes, D::max_message_bytes);
    EXPECT_EQ(client.max_nesting_depth, D::max_nesting_depth);
    EXPECT_EQ(client.max_queued_events, D::max_queued_events);
    EXPECT_EQ(client.max_list_pages, D::max_list_pages);
    EXPECT_EQ(client.max_list_items, D::max_list_items);
    EXPECT_EQ(client.peer_schema_depth, D::peer_schema_depth);
    EXPECT_EQ(engine.peer_schema_depth, D::peer_schema_depth);
    EXPECT_EQ(client.peer_pattern_length, D::peer_pattern_length);
    EXPECT_EQ(client.peer_pattern_memory, D::peer_pattern_memory);
    EXPECT_EQ(client.peer_patterns_per_schema, D::peer_patterns_per_schema);
    EXPECT_EQ(client.max_peer_schema_compiles, D::max_peer_schema_compiles);
    EXPECT_EQ(client.peer_max_evaluations, D::peer_max_evaluations);
    EXPECT_EQ(client.max_input_rounds, D::max_input_rounds);
    EXPECT_EQ(client.input_handler_timeout, D::input_handler_timeout);
    EXPECT_EQ(client.max_input_requests_per_round, D::max_input_requests_per_round);
    EXPECT_EQ(client.max_concurrent_server_requests, D::max_concurrent_server_requests);
    EXPECT_EQ(client.max_pending_requests, D::max_pending_requests);
    EXPECT_EQ(client.max_subscriptions, D::max_subscriptions);
    EXPECT_EQ(client.discover_probe_timeout, D::discover_probe_timeout);
    EXPECT_EQ(client.max_restarts, D::max_restarts);
    EXPECT_EQ(client.max_subscription_reopens, D::max_subscription_reopens);

    const neomcp::ChildProcess process;
    EXPECT_EQ(process.max_ignored_stdout_lines, P::max_ignored_stdout_lines);
    EXPECT_EQ(process.max_ignored_stdout_bytes, P::max_ignored_stdout_bytes);
    EXPECT_EQ(process.stderr_tail_bytes, P::stderr_tail_bytes);
    EXPECT_EQ(process.max_stderr_line_bytes, P::max_stderr_line_bytes);
    EXPECT_EQ(process.exit_grace, P::exit_grace);
    EXPECT_EQ(process.term_grace, P::term_grace);

    const neomcp::HttpEndpoint endpoint;
    EXPECT_EQ(endpoint.max_response_header_bytes, H::max_response_header_bytes);
    EXPECT_EQ(endpoint.max_session_id_bytes, H::max_session_id_bytes);
    EXPECT_EQ(endpoint.stream_idle_timeout, H::stream_idle_timeout);
    EXPECT_EQ(endpoint.max_retry_delay, H::max_retry_delay);
    EXPECT_EQ(endpoint.max_resumptions, H::max_resumptions);
    EXPECT_EQ(endpoint.max_redirects, H::max_redirects);
    EXPECT_EQ(endpoint.endpoint_timeout, H::endpoint_timeout);
    EXPECT_EQ(endpoint.connect_timeout, H::connect_timeout);
}

}  // namespace
