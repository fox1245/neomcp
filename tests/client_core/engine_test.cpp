// The client engine against scripted answers: opening contact, the corrective retry, cancellation, clocks, input
// requests, headers, bounds, the rules of the sink, and malformed messages.
#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "engine_rig.h"
#include "support.h"

namespace {

using namespace client_core_test;
using neomcp::CallFailureKind;
using neomcp::ContactPhase;
using neomcp::DeadlineAction;
using neomcp::DeadlineKind;
using neomcp::DiagnosticKind;
using neomcp::OutgoingKind;
using neomcp::RequestRef;
using neomcp::profile::Transport;

constexpr std::string_view newest = "2026-07-28";
constexpr std::string_view handshake_revision = "2025-11-25";

neomcp::EngineOptions every_revision() {
    neomcp::EngineOptions options;
    options.client_info.name = "engine-test";
    options.client_info.version = "1.0.0";
    return options;
}

neomcp::EngineOptions with_prior(neomcp::EngineOptions options, Transport transport, std::string_view revision,
                                 neomcp::ServerKind kind = neomcp::ServerKind::per_request) {
    options.prior = neomcp::ContactVerdict{kind, transport, std::string(revision)};
    return options;
}

neomcp::OutgoingCall list_call() {
    neomcp::OutgoingCall call;
    call.message.method = "tools/list";
    call.message.params = json::object();
    return call;
}

json supported(std::initializer_list<std::string_view> versions) {
    json list = json::array();
    for (const auto version : versions) list.push_back(std::string(version));
    return json{{"supported", list}, {"requested", std::string(newest)}};
}

neomcp::CallFailureKind failure_of(const std::optional<neomcp::RequestEnded>& ended) {
    if (!ended) throw std::runtime_error("the call did not end");
    const auto* failure = std::get_if<neomcp::CallFailure>(&ended->outcome);
    if (failure == nullptr) throw std::runtime_error("the call did not fail");
    return failure->kind;
}

std::size_t count_requests(const Rig& rig, std::string_view method) {
    std::size_t count = 0;
    for (const auto& request : rig.requests()) count += request["method"] == std::string(method) ? 1 : 0;
    return count;
}

// The first clock the engine started: the probe's or the handshake's when contact is opened.
neomcp::DeadlineChange first_start(Rig& rig) {
    for (const auto& change : rig.all<neomcp::DeadlineChange>()) {
        if (change.action == DeadlineAction::start) return change;
    }
    throw std::runtime_error("no clock was started");
}

std::optional<std::string> header(const neomcp::Outgoing& outgoing, std::string_view name) {
    for (const auto& field : outgoing.headers) {
        if (field.name == name) return field.value;
    }
    return std::nullopt;
}

const neomcp::Outgoing& last_output(const Rig& rig, OutgoingKind kind) {
    for (auto it = rig.out.rbegin(); it != rig.out.rend(); ++it) {
        if (it->kind == kind) return *it;
    }
    throw std::runtime_error("no output of that kind");
}

void open_handshake(Rig& rig, std::string_view revision) {
    rig.engine->open();
    rig.answer("initialize", initialize_result(revision));
    ASSERT_EQ(rig.engine->contact().phase, ContactPhase::open);
}

// ---- the probe on stdio, row by row

TEST(ClientEngineProbe, ADiscoveryResultMeansAServerThatNamesRevisionsPerRequest) {
    Rig rig(every_revision());
    rig.engine->open();
    EXPECT_EQ(rig.engine->contact().phase, ContactPhase::probing);
    const auto held = rig.send("tools/list");
    ASSERT_EQ(rig.requests().size(), 1u);  // nothing goes out before the probe's answer
    EXPECT_EQ(rig.requests()[0]["method"], "server/discover");
    EXPECT_EQ(rig.requests()[0]["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"], std::string(newest));
    rig.answer("server/discover", discover_result({newest, handshake_revision}));
    const auto& contact = rig.engine->contact();
    EXPECT_EQ(contact.phase, ContactPhase::open);
    ASSERT_TRUE(contact.verdict);
    EXPECT_EQ(contact.verdict->kind, neomcp::ServerKind::per_request);
    EXPECT_EQ(contact.verdict->revision, newest);
    ASSERT_TRUE(contact.revisions);
    EXPECT_EQ(contact.revisions->size(), 2u);
    EXPECT_EQ(count_requests(rig, "initialize"), 0u);
    EXPECT_EQ(count_requests(rig, "tools/list"), 1u);
    EXPECT_FALSE(rig.ended(held));
}

TEST(ClientEngineProbe, ADiscoveryThatListsOnlyHandshakeRevisionsLeadsToTheHandshake) {
    Rig rig(every_revision());
    rig.engine->open();
    rig.answer("server/discover", discover_result({"2025-06-18"}));
    EXPECT_EQ(rig.engine->contact().phase, ContactPhase::handshaking);
    ASSERT_EQ(count_requests(rig, "initialize"), 1u);
    EXPECT_EQ(rig.last("initialize")->first["params"]["protocolVersion"], "2025-06-18");
}

TEST(ClientEngineProbe, AnEmptyResultMeansAServerThatHandshakesOnTheSameProcess) {
    Rig rig(every_revision());
    rig.engine->open();
    rig.answer("server/discover", json::object());
    EXPECT_EQ(rig.engine->contact().phase, ContactPhase::handshaking);
    EXPECT_EQ(count_requests(rig, "initialize"), 1u);
    EXPECT_TRUE(rig.outputs(OutgoingKind::restart).empty());
    open_handshake(rig, handshake_revision);  // answers the handshake that is out
    EXPECT_EQ(rig.engine->contact().verdict->kind, neomcp::ServerKind::handshake);
    EXPECT_EQ(rig.engine->contact().verdict->revision, handshake_revision);
}

TEST(ClientEngineProbe, AMalformedDiscoveryResultIsDiagnosedAndTakenForAHandshakeServer) {
    Rig rig(every_revision());
    rig.engine->open();
    auto result = discover_result({newest});
    result["supportedVersions"] = "not a list";
    rig.answer("server/discover", result);
    EXPECT_EQ(rig.diagnostics(DiagnosticKind::malformed_contact).size(), 1u);
    EXPECT_EQ(count_requests(rig, "initialize"), 1u);
}

TEST(ClientEngineProbe, ARefusedRevisionIsProbedOnceMoreAndNeverHandshaken) {
    Rig rig(every_revision());
    rig.engine->open();
    const auto first = rig.last_id("server/discover");
    rig.refuse("server/discover", -32022, "Unsupported protocol version", supported({newest}));
    ASSERT_EQ(count_requests(rig, "server/discover"), 2u);
    EXPECT_NE(rig.last_id("server/discover"), first);
    EXPECT_EQ(count_requests(rig, "initialize"), 0u);
    rig.answer("server/discover", discover_result({newest}));
    EXPECT_EQ(rig.engine->contact().phase, ContactPhase::open);
}

TEST(ClientEngineProbe, ASecondRefusalOfTheProbeFailsContactWithoutAHandshake) {
    Rig rig(every_revision());
    rig.engine->open();
    const auto held = rig.send("tools/list");
    rig.refuse("server/discover", -32022, "Unsupported protocol version", supported({newest}));
    rig.refuse("server/discover", -32022, "Unsupported protocol version", supported({newest}));
    EXPECT_EQ(rig.engine->contact().phase, ContactPhase::failed);
    EXPECT_EQ(rig.engine->contact().failure->kind, neomcp::ContactFailureKind::no_common_revision);
    EXPECT_EQ(count_requests(rig, "initialize"), 0u);
    EXPECT_EQ(failure_of(rig.ended(held)), CallFailureKind::contact_failed);
}

TEST(ClientEngineProbe, ARefusalThatNamesOnlyHandshakeRevisionsFailsContact) {
    Rig rig(every_revision());
    rig.engine->open();
    rig.refuse("server/discover", -32022, "Unsupported protocol version", supported({handshake_revision}));
    EXPECT_EQ(rig.engine->contact().phase, ContactPhase::failed);
    EXPECT_EQ(count_requests(rig, "initialize"), 0u);
    ASSERT_EQ(rig.engine->contact().failure->server_revisions.size(), 1u);
}

TEST(ClientEngineProbe, AnotherErrorOfAPerRequestServerOpensAtTheProbedRevision) {
    Rig rig(every_revision());
    rig.engine->open();
    rig.refuse("server/discover", -32021, "Missing required client capability");
    const auto& contact = rig.engine->contact();
    EXPECT_EQ(contact.phase, ContactPhase::open);
    EXPECT_EQ(contact.verdict->revision, newest);
    EXPECT_FALSE(contact.revisions);
    EXPECT_EQ(count_requests(rig, "initialize"), 0u);
    EXPECT_TRUE(rig.diagnostics(DiagnosticKind::server_defect).empty());
}

TEST(ClientEngineProbe, AHeaderRefusalOpensAndIsReportedAsADefectOfTheServer) {
    Rig rig(every_revision());
    rig.engine->open();
    rig.refuse("server/discover", -32020, "Header mismatch");
    EXPECT_EQ(rig.engine->contact().phase, ContactPhase::open);
    EXPECT_EQ(rig.diagnostics(DiagnosticKind::server_defect).size(), 1u);
}

TEST(ClientEngineProbe, AnyOtherErrorMeansAHandshakeServer) {
    for (const int code : {-32601, -32602, -32600, -32603, -1}) {
        Rig rig(every_revision());
        rig.engine->open();
        rig.refuse("server/discover", code, "no");
        EXPECT_EQ(rig.engine->contact().phase, ContactPhase::handshaking) << code;
        EXPECT_EQ(count_requests(rig, "initialize"), 1u) << code;
    }
}

TEST(ClientEngineProbe, NoAnswerInTimeMeansAHandshakeServerAndALateAnswerIsUnknown) {
    auto options = every_revision();
    options.discover_probe_timeout = std::chrono::milliseconds{1234};
    Rig rig(options);
    rig.engine->open();
    const auto probe = first_start(rig);
    EXPECT_EQ(probe.duration, std::chrono::milliseconds{1234});
    const auto probe_id = rig.last_id("server/discover");
    rig.engine->expire(probe.deadline);
    EXPECT_EQ(rig.engine->contact().phase, ContactPhase::handshaking);
    EXPECT_EQ(count_requests(rig, "initialize"), 1u);
    EXPECT_TRUE(rig.outputs(OutgoingKind::notification).empty());  // nothing cancels the probe
    const auto late = neomcp::make_result(neomcp::RequestId(probe_id), discover_result({newest}));
    rig.engine->received(neomcp::connection_unit, neomcp::serialize(late));
    EXPECT_EQ(rig.diagnostics(DiagnosticKind::unknown_id).size(), 1u);
    EXPECT_EQ(rig.engine->contact().phase, ContactPhase::handshaking);
}

TEST(ClientEngineProbe, AProcessThatEndsFirstIsStartedOnceMoreForTheHandshake) {
    auto options = every_revision();
    options.max_restarts = 0;  // the start that belongs to the probe does not count
    Rig rig(options);
    rig.engine->open();
    const auto held = rig.send("tools/list");
    rig.engine->stream_event(neomcp::connection_unit, neomcp::ProcessExited{1});
    EXPECT_EQ(rig.engine->contact().phase, ContactPhase::lost);
    EXPECT_EQ(rig.outputs(OutgoingKind::restart).size(), 1u);
    EXPECT_EQ(count_requests(rig, "initialize"), 0u);
    rig.engine->transport_restarted();
    EXPECT_EQ(count_requests(rig, "initialize"), 1u);
    open_handshake(rig, handshake_revision);
    EXPECT_EQ(count_requests(rig, "tools/list"), 1u);
    EXPECT_FALSE(rig.ended(held));
}

TEST(ClientEngineProbe, OnlyHandshakeRevisionsMeanNoProbe) {
    Rig rig(only({"2025-11-25", "2025-06-18"}));
    rig.engine->open();
    ASSERT_EQ(rig.requests().size(), 1u);
    EXPECT_EQ(rig.requests()[0]["method"], "initialize");
    EXPECT_EQ(rig.requests()[0]["params"]["protocolVersion"], "2025-11-25");
}

TEST(ClientEngineProbe, OnlyRevisionsNamedPerRequestStillProbeFirst) {
    Rig rig(only({newest}));
    rig.engine->open();
    ASSERT_EQ(rig.requests().size(), 1u);
    EXPECT_EQ(rig.requests()[0]["method"], "server/discover");
    rig.answer("server/discover", json::object());
    EXPECT_EQ(rig.engine->contact().phase, ContactPhase::failed);
}

TEST(ClientEngineProbe, OverHttpTheStatusOfAnAnswerWithoutAMessageDecides) {
    const auto probe_ended = [](int status) {
        auto rig = std::make_unique<Rig>(every_revision(), Transport::streamable_http);
        rig->engine->open();
        const auto unit = rig->last_unit("server/discover");
        rig->engine->stream_event(unit, neomcp::ResponseHead{status, std::nullopt, {}});
        rig->engine->stream_event(unit, neomcp::StreamEnded{});
        return rig;
    };
    for (const int status : {400, 404, 405}) {
        const auto rig = probe_ended(status);
        EXPECT_EQ(rig->engine->contact().phase, ContactPhase::handshaking) << status;
        EXPECT_EQ(count_requests(*rig, "initialize"), 1u) << status;
    }
    for (const int status : {401, 403}) {
        const auto rig = probe_ended(status);
        EXPECT_EQ(rig->engine->contact().failure->kind, neomcp::ContactFailureKind::authorization_required) << status;
        EXPECT_EQ(count_requests(*rig, "initialize"), 0u) << status;
    }
    for (const int status : {500, 410}) {
        const auto rig = probe_ended(status);
        EXPECT_EQ(rig->engine->contact().failure->kind, neomcp::ContactFailureKind::http_status) << status;
        EXPECT_EQ(count_requests(*rig, "initialize"), 0u) << status;
    }
}

TEST(ClientEngineProbe, AHandshakeVerdictMetByAnErrorOfAPerRequestServerIsProbedOnce) {
    Rig rig(with_prior(every_revision(), Transport::stdio, handshake_revision, neomcp::ServerKind::handshake));
    rig.engine->open();
    ASSERT_EQ(rig.requests().size(), 1u);
    EXPECT_EQ(rig.requests()[0]["method"], "initialize");
    rig.refuse("initialize", -32021, "Missing required client capability");
    EXPECT_EQ(rig.engine->contact().phase, ContactPhase::probing);
    EXPECT_EQ(count_requests(rig, "server/discover"), 1u);
    rig.answer("server/discover", json::object());  // no discovery result: the handshake once more
    ASSERT_EQ(count_requests(rig, "initialize"), 2u);
    rig.refuse("initialize", -32021, "Missing required client capability");
    EXPECT_EQ(rig.engine->contact().failure->kind, neomcp::ContactFailureKind::handshake_refused);
    EXPECT_EQ(count_requests(rig, "server/discover"), 1u);
}

// ---- the corrective retry of a call

TEST(ClientEngineRetry, ARefusedRevisionIsSentOnceMoreWithANewIdEvenAtTheSameRevision) {
    Rig rig(with_prior(only({newest}), Transport::stdio, newest));
    rig.engine->open();
    const auto ref = rig.send("tools/list");
    const auto first = rig.last_id("tools/list");
    rig.refuse("tools/list", -32022, "Unsupported protocol version", supported({newest}));
    ASSERT_EQ(count_requests(rig, "tools/list"), 2u);
    EXPECT_NE(rig.last_id("tools/list"), first);
    EXPECT_FALSE(rig.ended(ref));
    rig.answer("tools/list", json{{"tools", json::array()}});
    EXPECT_TRUE(std::holds_alternative<neomcp::Answered>(rig.ended(ref)->outcome));
}

TEST(ClientEngineRetry, ASecondRefusalIsAnError) {
    Rig rig(with_prior(only({newest}), Transport::stdio, newest));
    rig.engine->open();
    const auto ref = rig.send("tools/list");
    rig.refuse("tools/list", -32022, "Unsupported protocol version", supported({newest}));
    rig.refuse("tools/list", -32022, "Unsupported protocol version", supported({newest}));
    EXPECT_EQ(failure_of(rig.ended(ref)), CallFailureKind::version_refused);
    EXPECT_EQ(count_requests(rig, "tools/list"), 2u);
}

TEST(ClientEngineRetry, ARefusalThatNamesOnlyHandshakeRevisionsNeverLeadsToTheHandshake) {
    Rig rig(with_prior(every_revision(), Transport::stdio, newest));
    rig.engine->open();
    const auto ref = rig.send("tools/list");
    rig.refuse("tools/list", -32022, "Unsupported protocol version", supported({handshake_revision}));
    EXPECT_EQ(failure_of(rig.ended(ref)), CallFailureKind::no_common_revision);
    EXPECT_EQ(count_requests(rig, "initialize"), 0u);
}

TEST(ClientEngineRetry, AnInvalidParamsAnswerUnderAHandshakeRevisionIsNoRefusal) {
    Rig rig(only({handshake_revision}));
    open_handshake(rig, handshake_revision);
    const auto ref = rig.send("tools/list");
    rig.refuse("tools/list", -32602, "Invalid params", supported({handshake_revision}));
    const auto ended = rig.ended(ref);
    ASSERT_TRUE(ended);
    ASSERT_TRUE(std::holds_alternative<neomcp::ErrorAnswered>(ended->outcome));
    EXPECT_EQ(std::get<neomcp::ErrorAnswered>(ended->outcome).error.code, -32602);
    EXPECT_EQ(count_requests(rig, "tools/list"), 1u);
}

// ---- the handshake

TEST(ClientEngineHandshake, AnInvalidParamsRefusalThatNamesACandidateIsRetriedOnce) {
    Rig rig(only({"2025-11-25", "2025-06-18"}));
    rig.engine->open();
    rig.refuse("initialize", -32602, "Unsupported protocol version", supported({"2025-06-18"}));
    ASSERT_EQ(count_requests(rig, "initialize"), 2u);
    EXPECT_EQ(rig.last("initialize")->first["params"]["protocolVersion"], "2025-06-18");
    rig.refuse("initialize", -32602, "Unsupported protocol version", supported({"2025-06-18"}));
    EXPECT_EQ(rig.engine->contact().phase, ContactPhase::failed);
    EXPECT_EQ(rig.outputs(OutgoingKind::close).size(), 1u);
}

TEST(ClientEngineHandshake, AnAnswerAtARevisionNoCandidateIsEndsTheTransport) {
    Rig rig(only({handshake_revision}));
    rig.engine->open();
    rig.answer("initialize", initialize_result("2025-06-18"));
    EXPECT_EQ(rig.engine->contact().failure->kind, neomcp::ContactFailureKind::unserved_revision);
    EXPECT_EQ(rig.engine->contact().failure->server_revisions, std::vector<std::string>{"2025-06-18"});
    EXPECT_EQ(rig.outputs(OutgoingKind::close).size(), 1u);
}

TEST(ClientEngineHandshake, TheReadyNotificationFollowsAndOnlyPingsGoBeforeTheAnswer) {
    Rig rig(only({handshake_revision}));
    rig.engine->open();
    const auto listed = rig.send("tools/list");
    const auto pinged = rig.send("ping");
    EXPECT_EQ(count_requests(rig, "tools/list"), 0u);
    EXPECT_EQ(count_requests(rig, "ping"), 1u);
    rig.answer("initialize", initialize_result(handshake_revision));
    const auto notifications = rig.outputs(OutgoingKind::notification);
    ASSERT_EQ(notifications.size(), 1u);
    EXPECT_EQ(json::parse(notifications[0].line)["method"], "notifications/initialized");
    EXPECT_EQ(count_requests(rig, "tools/list"), 1u);
    rig.answer("ping", json::object());
    EXPECT_TRUE(std::holds_alternative<neomcp::Answered>(rig.ended(pinged)->outcome));
    EXPECT_FALSE(rig.ended(listed));
}

TEST(ClientEngineHandshake, TheHandshakeIsNeverCancelledAndItsExpiryEndsTheTransport) {
    Rig rig(only({handshake_revision}));
    rig.engine->open();
    const auto handshake = first_start(rig);
    const auto ref = std::get<RequestRef>(handshake.deadline.owner);
    const auto written = rig.out.size();
    rig.engine->cancel(ref, "no");
    EXPECT_EQ(rig.out.size(), written);
    EXPECT_EQ(rig.engine->contact().phase, ContactPhase::handshaking);
    rig.engine->expire(handshake.deadline);
    EXPECT_EQ(rig.engine->contact().failure->kind, neomcp::ContactFailureKind::handshake_timed_out);
    EXPECT_EQ(rig.outputs(OutgoingKind::close).size(), 1u);
    EXPECT_TRUE(rig.outputs(OutgoingKind::notification).empty());
}

TEST(ClientEngineHandshake, TheLogLevelIsSetAfterTheHandshakeWhenTheServerLogs) {
    auto options = only({handshake_revision});
    options.log_level = neomcp::LogLevel::warning;
    Rig rig(options);
    rig.engine->open();
    auto result = initialize_result(handshake_revision);
    result["capabilities"] = json{{"logging", json::object()}};
    rig.answer("initialize", result);
    ASSERT_EQ(count_requests(rig, "logging/setLevel"), 1u);
    EXPECT_EQ(rig.last("logging/setLevel")->first["params"]["level"], "warning");
    rig.refuse("logging/setLevel", -32602, "no");
    EXPECT_EQ(rig.diagnostics(DiagnosticKind::level_refused).size(), 1u);
    EXPECT_TRUE(rig.all<neomcp::RequestEnded>().empty());
}

// ---- cancellation and expiry

TEST(ClientEngineCancel, OnStdioACancellationNotificationNamesTheRequest) {
    Rig rig(with_prior(only({newest}), Transport::stdio, newest));
    rig.engine->open();
    const auto ref = rig.send("tools/list");
    const auto id = rig.last_id("tools/list");
    rig.engine->cancel(ref, "the user stopped");
    EXPECT_EQ(failure_of(rig.ended(ref)), CallFailureKind::cancelled);
    const auto cancelled = json::parse(last_output(rig, OutgoingKind::notification).line);
    EXPECT_EQ(cancelled["method"], "notifications/cancelled");
    EXPECT_EQ(cancelled["params"]["requestId"], json(id));
    EXPECT_EQ(cancelled["params"]["reason"], "the user stopped");
    EXPECT_EQ(rig.engine->pending(), 0u);
}

TEST(ClientEngineCancel, WhereClosingTheExchangeCancelsNothingIsSent) {
    Rig rig(with_prior(only({newest}), Transport::streamable_http, newest), Transport::streamable_http);
    rig.engine->open();
    const auto ref = rig.send("tools/list");
    const auto unit = rig.last_unit("tools/list");
    rig.engine->cancel(ref);
    EXPECT_EQ(failure_of(rig.ended(ref)), CallFailureKind::cancelled);
    EXPECT_TRUE(rig.outputs(OutgoingKind::notification).empty());
    const auto ended = rig.outputs(OutgoingKind::end_unit);
    ASSERT_EQ(ended.size(), 1u);
    EXPECT_EQ(ended[0].unit, unit);
}

TEST(ClientEngineCancel, OnHandshakeRevisionsOverHttpTheNotificationGoesAndTheExchangeIsDropped) {
    Rig rig(only({handshake_revision}), Transport::streamable_http);
    open_handshake(rig, handshake_revision);
    const auto ref = rig.send("tools/list");
    const auto unit = rig.last_unit("tools/list");
    rig.engine->cancel(ref);
    const auto& notification = last_output(rig, OutgoingKind::notification);
    EXPECT_EQ(json::parse(notification.line)["method"], "notifications/cancelled");
    EXPECT_NE(notification.unit, unit);
    EXPECT_EQ(last_output(rig, OutgoingKind::end_unit).unit, unit);
}

TEST(ClientEngineCancel, ExpiryCancelsAsTheTransportPrescribesAndEndsTimedOut) {
    Rig rig(with_prior(only({newest}), Transport::stdio, newest));
    rig.engine->open();
    neomcp::OutgoingCall call;
    call.message.method = "tools/list";
    call.timeout = std::chrono::milliseconds{250};
    const auto ref = rig.send(call);
    const auto wait = rig.clock(ref, DeadlineKind::wait);
    ASSERT_TRUE(wait);
    EXPECT_EQ(wait->duration, std::chrono::milliseconds{250});
    rig.engine->expire(wait->deadline);
    EXPECT_EQ(failure_of(rig.ended(ref)), CallFailureKind::timed_out);
    EXPECT_EQ(json::parse(last_output(rig, OutgoingKind::notification).line)["method"], "notifications/cancelled");
    EXPECT_EQ(rig.clock(ref, DeadlineKind::wait)->action, DeadlineAction::stop);
}

TEST(ClientEngineCancel, AHeldCallThatIsCancelledEndsWithNothingWritten) {
    Rig rig(every_revision());
    rig.engine->open();
    const auto written = rig.out.size();
    const auto ref = rig.send("tools/list");
    rig.engine->cancel(ref);
    EXPECT_EQ(rig.out.size(), written);
    EXPECT_EQ(failure_of(rig.ended(ref)), CallFailureKind::cancelled);
    rig.answer("server/discover", discover_result({newest}));
    EXPECT_EQ(count_requests(rig, "tools/list"), 0u);
}

// ---- clocks

TEST(ClientEngineClock, AStaleDeadlineIsIgnored) {
    auto options = with_prior(only({handshake_revision}), Transport::stdio, handshake_revision,
                              neomcp::ServerKind::handshake);
    options.input.roots = true;
    Rig rig(options);
    open_handshake(rig, handshake_revision);
    neomcp::OutgoingCall call;
    call.message.method = "tools/list";
    call.progress = true;
    call.reset_timeout_on_progress = true;
    const auto ref = rig.send(call);
    const auto first = *rig.clock(ref, DeadlineKind::wait);
    // Progress starts the wait again: the first stretch is stale.
    rig.engine->received(neomcp::connection_unit,
                         neomcp::serialize(neomcp::make_notification(
                             "notifications/progress", json{{"progressToken", ref.value}, {"progress", 1}})));
    const auto second = *rig.clock(ref, DeadlineKind::wait);
    ASSERT_NE(first.deadline.epoch, second.deadline.epoch);
    rig.engine->expire(first.deadline);
    EXPECT_FALSE(rig.ended(ref));
    // A server's request pauses the clock: the stretch that ran before is stale, and so is a paused clock.
    rig.engine->received(neomcp::connection_unit, R"({"jsonrpc":"2.0","id":"s1","method":"roots/list"})");
    const auto paused = *rig.clock(ref, DeadlineKind::wait);
    EXPECT_EQ(paused.action, DeadlineAction::pause);
    rig.engine->expire(second.deadline);
    rig.engine->expire(paused.deadline);
    EXPECT_FALSE(rig.ended(ref));
    const auto asked = rig.all<neomcp::ServerAsked>();
    ASSERT_EQ(asked.size(), 1u);
    rig.engine->answer(asked[0].ref, neomcp::InputResponse(neomcp::RootsAnswer{}));
    const auto resumed = *rig.clock(ref, DeadlineKind::wait);
    EXPECT_EQ(resumed.action, DeadlineAction::resume);
    // A stopped clock: the call ended, and its last stretch is stale too.
    rig.answer("tools/list", json{{"tools", json::array()}});
    ASSERT_TRUE(rig.ended(ref));
    const auto events = rig.drain().size();
    rig.engine->expire(resumed.deadline);
    EXPECT_EQ(rig.drain().size(), events);
    EXPECT_EQ(rig.all<neomcp::RequestEnded>().size(), 1u);
}

TEST(ClientEngineClock, ProgressReachesTheCallAndStartsItsWaitAgainButNotItsTotal) {
    auto options = with_prior(only({newest}), Transport::stdio, newest);
    options.max_total_timeout = std::chrono::milliseconds{9000};
    Rig rig(options);
    rig.engine->open();
    neomcp::OutgoingCall call;
    call.message.method = "tools/list";
    call.progress = true;
    call.reset_timeout_on_progress = true;
    const auto ref = rig.send(call);
    EXPECT_EQ(rig.requests().back()["params"]["_meta"]["progressToken"], json(ref.value));
    const auto total = *rig.clock(ref, DeadlineKind::total);
    EXPECT_EQ(total.duration, std::chrono::milliseconds{9000});
    rig.engine->received(neomcp::connection_unit,
                         neomcp::serialize(neomcp::make_notification(
                             "notifications/progress",
                             json{{"progressToken", ref.value}, {"progress", 2}, {"total", 4}, {"message", "half"}})));
    const auto progress = rig.all<neomcp::ProgressEvent>();
    ASSERT_EQ(progress.size(), 1u);
    EXPECT_EQ(progress[0].request, ref);
    EXPECT_EQ(progress[0].progress, 2);
    EXPECT_EQ(progress[0].total, 4);
    EXPECT_EQ(progress[0].message, "half");
    EXPECT_EQ(rig.clock(ref, DeadlineKind::wait)->action, DeadlineAction::start);
    EXPECT_EQ(rig.clock(ref, DeadlineKind::total)->deadline, total.deadline);
    rig.engine->expire(total.deadline);
    EXPECT_EQ(failure_of(rig.ended(ref)), CallFailureKind::timed_out);
}

TEST(ClientEngineClock, ProgressWithoutResetLeavesTheWaitAlone) {
    Rig rig(with_prior(only({newest}), Transport::stdio, newest));
    rig.engine->open();
    neomcp::OutgoingCall call;
    call.message.method = "tools/list";
    call.progress = true;
    const auto ref = rig.send(call);
    const auto wait = *rig.clock(ref, DeadlineKind::wait);
    EXPECT_FALSE(rig.clock(ref, DeadlineKind::total));
    rig.engine->received(neomcp::connection_unit,
                         neomcp::serialize(neomcp::make_notification(
                             "notifications/progress", json{{"progressToken", ref.value}, {"progress", 1}})));
    EXPECT_EQ(rig.all<neomcp::ProgressEvent>().size(), 1u);
    EXPECT_EQ(rig.clock(ref, DeadlineKind::wait)->deadline, wait.deadline);
}

// ---- requests of the server, and the seam of input handlers

TEST(ClientEngineInput, APingOfTheServerIsAnsweredAtOnce) {
    Rig rig(only({handshake_revision}));
    open_handshake(rig, handshake_revision);
    rig.engine->received(neomcp::connection_unit, R"({"jsonrpc":"2.0","id":7,"method":"ping"})");
    const auto answer = json::parse(last_output(rig, OutgoingKind::response).line);
    EXPECT_EQ(answer["id"], 7);
    EXPECT_EQ(answer["result"], json::object());
}

TEST(ClientEngineInput, ARequestWithoutAHandlerIsRefusedAsTheRevisionPrescribes) {
    Rig rig(only({handshake_revision}));
    open_handshake(rig, handshake_revision);
    rig.engine->received(neomcp::connection_unit, R"({"jsonrpc":"2.0","id":"r","method":"roots/list"})");
    const auto answer = json::parse(last_output(rig, OutgoingKind::response).line);
    EXPECT_EQ(answer["id"], "r");
    EXPECT_EQ(answer["error"]["code"], -32601);
    EXPECT_TRUE(rig.all<neomcp::ServerAsked>().empty());
}

TEST(ClientEngineInput, ARequestWithAHandlerPausesThePendingCallsUntilItIsAnswered) {
    auto options = only({handshake_revision});
    options.input.roots = true;
    Rig rig(options);
    open_handshake(rig, handshake_revision);
    EXPECT_TRUE(rig.last("initialize")->first["params"]["capabilities"].contains("roots"));
    const auto a = rig.send("tools/list");
    const auto b = rig.send("prompts/list");
    rig.engine->received(neomcp::connection_unit, R"({"jsonrpc":"2.0","id":"r","method":"roots/list"})");
    const auto asked = rig.all<neomcp::ServerAsked>();
    ASSERT_EQ(asked.size(), 1u);
    EXPECT_EQ(asked[0].method, "roots/list");
    EXPECT_FALSE(asked[0].call);  // stdio does not say which call it belongs to
    EXPECT_EQ(asked[0].paused, (std::vector<RequestRef>{a, b}));
    EXPECT_EQ(rig.clock(a, DeadlineKind::wait)->action, DeadlineAction::pause);
    EXPECT_EQ(rig.clock(b, DeadlineKind::wait)->action, DeadlineAction::pause);
    EXPECT_EQ(rig.clock(asked[0].ref, DeadlineKind::input)->action, DeadlineAction::start);
    neomcp::RootsAnswer roots;
    roots.roots.push_back(neomcp::Root{"file:///work", std::string("work"), std::nullopt});
    rig.engine->answer(asked[0].ref, neomcp::InputResponse(roots));
    const auto answer = json::parse(last_output(rig, OutgoingKind::response).line);
    EXPECT_EQ(answer["id"], "r");
    EXPECT_EQ(answer["result"]["roots"][0]["uri"], "file:///work");
    EXPECT_EQ(rig.clock(a, DeadlineKind::wait)->action, DeadlineAction::resume);
    EXPECT_EQ(rig.clock(asked[0].ref, DeadlineKind::input)->action, DeadlineAction::stop);
    // A second answer is ignored.
    const auto written = rig.out.size();
    rig.engine->answer(asked[0].ref, neomcp::InputResponse(roots));
    EXPECT_EQ(rig.out.size(), written);
}

TEST(ClientEngineInput, AnUnansweredRequestIsAnsweredByTheEngineWhenItsClockLapses) {
    auto options = only({handshake_revision});
    options.input.roots = true;
    Rig rig(options);
    open_handshake(rig, handshake_revision);
    const auto a = rig.send("tools/list");
    rig.engine->received(neomcp::connection_unit, R"({"jsonrpc":"2.0","id":"r","method":"roots/list"})");
    const auto asked = rig.all<neomcp::ServerAsked>().at(0);
    rig.engine->expire(rig.clock(asked.ref, DeadlineKind::input)->deadline);
    const auto answer = json::parse(last_output(rig, OutgoingKind::response).line);
    EXPECT_EQ(answer["error"]["code"], -32603);
    ASSERT_EQ(rig.all<neomcp::ServerAskEnded>().size(), 1u);
    EXPECT_FALSE(rig.all<neomcp::ServerAskEnded>()[0].cancelled_by_server);
    EXPECT_EQ(rig.clock(a, DeadlineKind::wait)->action, DeadlineAction::resume);
}

TEST(ClientEngineInput, TheServerCancellingItsRequestStopsTheHandlerWithoutAnAnswer) {
    auto options = only({handshake_revision});
    options.input.roots = true;
    Rig rig(options);
    open_handshake(rig, handshake_revision);
    rig.engine->received(neomcp::connection_unit, R"({"jsonrpc":"2.0","id":"r","method":"roots/list"})");
    const auto asked = rig.all<neomcp::ServerAsked>().at(0);
    const auto written = rig.out.size();
    rig.engine->received(neomcp::connection_unit,
                         R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":"r"}})");
    ASSERT_EQ(rig.all<neomcp::ServerAskEnded>().size(), 1u);
    EXPECT_TRUE(rig.all<neomcp::ServerAskEnded>()[0].cancelled_by_server);
    rig.engine->answer(asked.ref, neomcp::InputResponse(neomcp::RootsAnswer{}));
    EXPECT_EQ(rig.out.size(), written);
}

TEST(ClientEngineInput, RequestsOfTheServerAreBoundedAndPingsDoNotCount) {
    auto options = only({handshake_revision});
    options.input.roots = true;
    options.max_concurrent_server_requests = 2;
    Rig rig(options);
    open_handshake(rig, handshake_revision);
    for (int i = 0; i < 2; ++i) {
        rig.engine->received(neomcp::connection_unit,
                             R"({"jsonrpc":"2.0","id":)" + std::to_string(100 + i) + R"(,"method":"roots/list"})");
    }
    EXPECT_EQ(rig.all<neomcp::ServerAsked>().size(), 2u);
    rig.engine->received(neomcp::connection_unit, R"({"jsonrpc":"2.0","id":9,"method":"ping"})");
    EXPECT_EQ(json::parse(last_output(rig, OutgoingKind::response).line)["result"], json::object());
    rig.engine->received(neomcp::connection_unit, R"({"jsonrpc":"2.0","id":102,"method":"roots/list"})");
    const auto busy = json::parse(last_output(rig, OutgoingKind::response).line);
    EXPECT_EQ(busy["id"], 102);
    EXPECT_EQ(busy["error"]["code"], -32603);
    EXPECT_EQ(rig.all<neomcp::ServerAsked>().size(), 2u);
}

TEST(ClientEngineInput, ARevisionWhoseServersSendNoRequestsGetsNoAnswer) {
    Rig rig(with_prior(only({newest}), Transport::stdio, newest));
    rig.engine->open();
    rig.engine->received(neomcp::connection_unit, R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
    EXPECT_TRUE(rig.outputs(OutgoingKind::response).empty());
    EXPECT_EQ(rig.diagnostics(DiagnosticKind::malformed_message).size(), 1u);
}

// A result of a call that asks for a form, with a state to echo.
json asking_result(std::string_view state) {
    return json::parse(R"({"resultType":"input_required","requestState":")" + std::string(state) +
                       R"(","inputRequests":{"confirm":{"method":"elicitation/create","params":{"mode":"form",
                       "message":"Sure?","requestedSchema":{"type":"object",
                       "properties":{"ok":{"type":"boolean"}}}}}}})");
}

neomcp::OutgoingCall tool_call() {
    neomcp::OutgoingCall call;
    call.message.method = "tools/call";
    call.message.params = json{{"name", "delete"}, {"arguments", json::object()}};
    return call;
}

TEST(ClientEngineRounds, WithoutAHandlerTheAskingIsTheCallsAnswer) {
    Rig rig(with_prior(only({newest}), Transport::stdio, newest));
    rig.engine->open();
    const auto ref = rig.send(tool_call());
    rig.answer("tools/call", asking_result("s1"));
    const auto ended = rig.ended(ref);
    ASSERT_TRUE(ended);
    const auto* asked = std::get_if<neomcp::AskedForInput>(&ended->outcome);
    ASSERT_NE(asked, nullptr);
    ASSERT_EQ(asked->asking.requests.size(), 1u);
    EXPECT_EQ(asked->asking.requests[0].key, "confirm");
    EXPECT_EQ(asked->asking.state, json("s1"));
    EXPECT_TRUE(rig.all<neomcp::InputRoundStarted>().empty());
}

TEST(ClientEngineRounds, WithAHandlerTheCallIsRetriedWithTheAnswersAndTheStateUnderANewId) {
    auto options = with_prior(only({newest}), Transport::stdio, newest);
    options.input.form = true;
    Rig rig(options);
    rig.engine->open();
    const auto ref = rig.send(tool_call());
    const auto first = rig.last_id("tools/call");
    rig.answer("tools/call", asking_result("s1"));
    const auto rounds = rig.all<neomcp::InputRoundStarted>();
    ASSERT_EQ(rounds.size(), 1u);
    EXPECT_EQ(rounds[0].round, 1u);
    EXPECT_EQ(rig.clock(ref, DeadlineKind::wait)->action, DeadlineAction::pause);
    EXPECT_EQ(rig.clock(ref, DeadlineKind::input)->action, DeadlineAction::start);
    neomcp::InputResponses answers;
    answers.emplace("confirm", neomcp::ElicitationAnswer{neomcp::ElicitationAction::accept, json{{"ok", true}}});
    rig.engine->answer_round(ref, answers);
    const auto retry = rig.last("tools/call")->first;
    EXPECT_NE(retry["id"], json(first));
    EXPECT_EQ(retry["params"]["requestState"], "s1");
    EXPECT_EQ(retry["params"]["inputResponses"]["confirm"]["action"], "accept");
    EXPECT_EQ(retry["params"]["name"], "delete");
    EXPECT_EQ(rig.clock(ref, DeadlineKind::wait)->action, DeadlineAction::resume);
    EXPECT_EQ(rig.clock(ref, DeadlineKind::input)->action, DeadlineAction::stop);
    rig.answer("tools/call", json{{"content", json::array()}, {"resultType", "complete"}});
    EXPECT_TRUE(std::holds_alternative<neomcp::Answered>(rig.ended(ref)->outcome));
}

TEST(ClientEngineRounds, RoundsAreBoundedAndARoundThatIsNotAnsweredInTimeFails) {
    auto options = with_prior(only({newest}), Transport::stdio, newest);
    options.input.form = true;
    options.max_input_rounds = 2;
    Rig rig(options);
    rig.engine->open();
    const auto ref = rig.send(tool_call());
    neomcp::InputResponses answers;
    answers.emplace("confirm", neomcp::ElicitationAnswer{neomcp::ElicitationAction::decline, json::object()});
    for (int round = 1; round <= 2; ++round) {
        rig.answer("tools/call", asking_result("s" + std::to_string(round)));
        ASSERT_EQ(rig.all<neomcp::InputRoundStarted>().size(), static_cast<std::size_t>(round));
        rig.engine->answer_round(ref, answers);
    }
    rig.answer("tools/call", asking_result("s3"));
    EXPECT_EQ(failure_of(rig.ended(ref)), CallFailureKind::input_rounds_exceeded);

    const auto other = rig.send(tool_call());
    rig.answer("tools/call", asking_result("t"));
    rig.engine->expire(rig.clock(other, DeadlineKind::input)->deadline);
    EXPECT_EQ(failure_of(rig.ended(other)), CallFailureKind::input_timed_out);
}

TEST(ClientEngineRounds, AResultKindTheClientDoesNotKnowIsMalformed) {
    Rig rig(with_prior(only({newest}), Transport::stdio, newest));
    rig.engine->open();
    const auto ref = rig.send("tools/list");
    rig.answer("tools/list", json{{"tools", json::array()}, {"resultType", "later"}});
    EXPECT_EQ(failure_of(rig.ended(ref)), CallFailureKind::malformed_answer);
}

// ---- headers

TEST(ClientEngineHeaders, PerRequestRevisionsPutTheirHeadersOnEveryRequestAsTheProfileSays) {
    Rig rig(with_prior(only({newest}), Transport::streamable_http, newest), Transport::streamable_http);
    rig.engine->open();
    rig.send("tools/list");
    const auto& listed = last_output(rig, OutgoingKind::request);
    EXPECT_EQ(header(listed, "MCP-Protocol-Version"), std::string(newest));
    EXPECT_EQ(header(listed, "Mcp-Method"), "tools/list");
    EXPECT_FALSE(header(listed, "Mcp-Name"));
    neomcp::OutgoingCall call;
    call.message.method = "tools/call";
    call.message.params = json{{"name", "find"}, {"arguments", json{{"n", 5}, {"s", " padded"}, {"b", true}}}};
    call.input_schema = json::parse(R"({"type":"object","properties":{
        "n":{"type":"integer","x-mcp-header":"N"},
        "s":{"type":"string","x-mcp-header":"S"},
        "b":{"type":"boolean","x-mcp-header":"B"},
        "absent":{"type":"string","x-mcp-header":"Absent"}}})");
    rig.send(call);
    const auto& called = last_output(rig, OutgoingKind::request);
    EXPECT_EQ(header(called, "Mcp-Name"), "find");
    EXPECT_EQ(header(called, "Mcp-Param-N"), "5");
    EXPECT_EQ(header(called, "Mcp-Param-S"), "=?base64?IHBhZGRlZA==?=");
    EXPECT_EQ(header(called, "Mcp-Param-B"), "true");
    EXPECT_FALSE(header(called, "Mcp-Param-Absent"));
}

TEST(ClientEngineHeaders, AnArgumentThatCannotBeMirroredFailsTheCallBeforeItGoesOut) {
    Rig rig(with_prior(only({newest}), Transport::streamable_http, newest), Transport::streamable_http);
    rig.engine->open();
    neomcp::OutgoingCall call;
    call.message.method = "tools/call";
    call.message.params = json{{"name", "find"}, {"arguments", json{{"n", 9007199254740992LL}}}};
    call.input_schema = json::parse(R"({"type":"object","properties":{"n":{"type":"integer","x-mcp-header":"N"}}})");
    const auto sent = rig.engine->send(call);
    ASSERT_TRUE(std::holds_alternative<neomcp::CallFailure>(sent));
    EXPECT_EQ(std::get<neomcp::CallFailure>(sent).kind, CallFailureKind::header_value);
    EXPECT_TRUE(rig.outputs(OutgoingKind::request).empty());
    EXPECT_EQ(rig.engine->pending(), 0u);
}

TEST(ClientEngineHeaders, HandshakeRevisionsPutTheVersionHeaderOnEveryOutputAfterTheHandshake) {
    Rig rig(only({handshake_revision}), Transport::streamable_http);
    rig.engine->open();
    EXPECT_FALSE(header(last_output(rig, OutgoingKind::request), "MCP-Protocol-Version"));
    rig.answer("initialize", initialize_result(handshake_revision));
    EXPECT_EQ(header(last_output(rig, OutgoingKind::notification), "MCP-Protocol-Version"),
              std::string(handshake_revision));
    const auto ref = rig.send("tools/list");
    EXPECT_EQ(header(last_output(rig, OutgoingKind::request), "MCP-Protocol-Version"), std::string(handshake_revision));
    rig.engine->cancel(ref);
    EXPECT_EQ(header(last_output(rig, OutgoingKind::notification), "MCP-Protocol-Version"),
              std::string(handshake_revision));
}

TEST(ClientEngineHeaders, StdioCarriesNoHeaders) {
    Rig rig(with_prior(only({newest}), Transport::stdio, newest));
    rig.engine->open();
    rig.send("tools/list");
    EXPECT_TRUE(last_output(rig, OutgoingKind::request).headers.empty());
    EXPECT_EQ(last_output(rig, OutgoingKind::request).unit, neomcp::connection_unit);
}

// ---- HTTP exchanges

TEST(ClientEngineHttp, AnExchangeThatEndsBeforeItsAnswerLosesTheCall) {
    Rig rig(with_prior(only({newest}), Transport::streamable_http, newest), Transport::streamable_http);
    rig.engine->open();
    const auto lost = rig.send("tools/list");
    rig.engine->stream_event(rig.last_unit("tools/list"),
                             neomcp::ResponseHead{200, std::string("text/event-stream"), {}});
    rig.engine->stream_event(rig.last_unit("tools/list"), neomcp::StreamEnded{});
    EXPECT_EQ(failure_of(rig.ended(lost)), CallFailureKind::stream_lost);
    const auto refused = rig.send("tools/list");
    rig.engine->stream_event(rig.last_unit("tools/list"), neomcp::ResponseHead{500, std::nullopt, {}});
    rig.engine->stream_event(rig.last_unit("tools/list"), neomcp::StreamEnded{});
    EXPECT_EQ(failure_of(rig.ended(refused)), CallFailureKind::http_status);
    const auto typed = rig.send("tools/list");
    rig.engine->stream_event(rig.last_unit("tools/list"), neomcp::ResponseHead{200, std::string("text/html"), {}});
    EXPECT_EQ(failure_of(rig.ended(typed)), CallFailureKind::bad_content_type);
    const auto denied = rig.send("tools/list");
    rig.engine->stream_event(rig.last_unit("tools/list"), neomcp::ResponseHead{401, std::nullopt, {}});
    EXPECT_EQ(failure_of(rig.ended(denied)), CallFailureKind::authorization_required);
    const auto undelivered = rig.send("tools/list");
    rig.engine->stream_event(rig.last_unit("tools/list"), neomcp::SendFailed{"refused"});
    EXPECT_EQ(failure_of(rig.ended(undelivered)), CallFailureKind::send_failed);
}

TEST(ClientEngineHttp, TheAnswerEndsItsExchange) {
    Rig rig(with_prior(only({newest}), Transport::streamable_http, newest), Transport::streamable_http);
    rig.engine->open();
    const auto ref = rig.send("tools/list");
    const auto unit = rig.last_unit("tools/list");
    rig.answer("tools/list", json{{"tools", json::array()}});
    ASSERT_TRUE(rig.ended(ref));
    ASSERT_EQ(rig.outputs(OutgoingKind::end_unit).size(), 1u);
    EXPECT_EQ(rig.outputs(OutgoingKind::end_unit)[0].unit, unit);
    // Input on an exchange that ended is ignored.
    EXPECT_EQ(rig.engine->received(unit, "{}"), neomcp::Reception::ignored);
}

// ---- bounds, each at its edge

TEST(ClientEngineBounds, PendingCallsAreBoundedAndTheEnginesOwnRequestsDoNotCount) {
    auto options = every_revision();
    options.max_pending_requests = 3;
    Rig rig(options);
    rig.engine->open();  // the probe is out and does not count
    for (int i = 0; i < 3; ++i) rig.send("tools/list");
    EXPECT_EQ(rig.engine->pending(), 3u);
    const auto refused = rig.engine->send(list_call());
    ASSERT_TRUE(std::holds_alternative<neomcp::CallFailure>(refused));
    EXPECT_EQ(std::get<neomcp::CallFailure>(refused).kind, CallFailureKind::too_many_pending);
    rig.answer("server/discover", discover_result({newest}));
    EXPECT_EQ(count_requests(rig, "tools/list"), 3u);
    rig.answer("tools/list", json{{"tools", json::array()}});
    EXPECT_EQ(rig.engine->pending(), 2u);
    rig.send("tools/list");
    EXPECT_EQ(rig.engine->pending(), 3u);
}

TEST(ClientEngineBounds, AFullQueueDropsOnlyWhatMayBeDroppedAndKeepsTheOrder) {
    auto options = with_prior(only({newest}), Transport::stdio, newest);
    options.max_queued_events = 3;
    Rig rig(options);
    rig.engine->open();  // one contact change
    const auto unknown = neomcp::serialize(neomcp::make_result(neomcp::RequestId(999), json::object()));
    rig.engine->received(neomcp::connection_unit, unknown);
    rig.engine->received(neomcp::connection_unit, unknown);  // the queue holds three now
    rig.engine->received(neomcp::connection_unit, unknown);  // dropped
    neomcp::OutgoingCall call;
    call.message.method = "tools/list";
    call.progress = true;
    const auto ref = rig.send(call);  // a clock change, never dropped
    // Dropped:
    rig.engine->received(neomcp::connection_unit,
                         neomcp::serialize(neomcp::make_notification(
                             "notifications/progress", json{{"progressToken", ref.value}, {"progress", 1}})));
    rig.engine->received(neomcp::connection_unit, R"({"jsonrpc":"2.0","method":"notifications/message",
        "params":{"level":"info","data":"x"}})");  // dropped
    rig.answer("tools/list", json{{"tools", json::array()}});  // a clock change and the end, never dropped
    EXPECT_EQ(rig.engine->dropped_events(), 3u);
    const auto& events = rig.drain();
    ASSERT_EQ(events.size(), 6u);
    EXPECT_TRUE(std::holds_alternative<neomcp::ContactChanged>(events[0]));
    EXPECT_TRUE(std::holds_alternative<neomcp::Diagnostic>(events[1]));
    EXPECT_TRUE(std::holds_alternative<neomcp::Diagnostic>(events[2]));
    ASSERT_TRUE(std::holds_alternative<neomcp::DeadlineChange>(events[3]));
    EXPECT_EQ(std::get<neomcp::DeadlineChange>(events[3]).action, DeadlineAction::start);
    ASSERT_TRUE(std::holds_alternative<neomcp::DeadlineChange>(events[4]));
    EXPECT_EQ(std::get<neomcp::DeadlineChange>(events[4]).action, DeadlineAction::stop);
    ASSERT_TRUE(std::holds_alternative<neomcp::RequestEnded>(events[5]));
    // With room again, droppable events are queued again.
    rig.engine->received(neomcp::connection_unit, unknown);
    EXPECT_EQ(rig.engine->dropped_events(), 3u);
    EXPECT_EQ(rig.diagnostics(DiagnosticKind::unknown_id).size(), 3u);
}

TEST(ClientEngineBounds, AFullQueueKeepsTheRequestsOfTheServerAndItsEnds) {
    auto options = only({handshake_revision});
    options.input.roots = true;
    options.max_queued_events = 2;
    Rig rig(options);
    open_handshake(rig, handshake_revision);  // more than two events wait: the queue is full
    const auto unknown = neomcp::serialize(neomcp::make_result(neomcp::RequestId(999), json::object()));
    rig.engine->received(neomcp::connection_unit, unknown);
    rig.engine->received(neomcp::connection_unit, R"({"jsonrpc":"2.0","id":"r","method":"roots/list"})");
    rig.engine->received(neomcp::connection_unit, R"({"jsonrpc":"2.0","method":"notifications/message",
        "params":{"level":"info","data":"x"}})");
    rig.engine->received(neomcp::connection_unit,
                         R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":"r"}})");
    EXPECT_EQ(rig.engine->dropped_events(), 2u);
    std::vector<std::size_t> asked;
    std::vector<std::size_t> ended;
    const auto& events = rig.drain();
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (std::holds_alternative<neomcp::ServerAsked>(events[i])) asked.push_back(i);
        if (std::holds_alternative<neomcp::ServerAskEnded>(events[i])) ended.push_back(i);
        EXPECT_FALSE(std::holds_alternative<neomcp::Diagnostic>(events[i])) << i;
        EXPECT_FALSE(std::holds_alternative<neomcp::LogEvent>(events[i])) << i;
    }
    ASSERT_EQ(asked.size(), 1u);
    ASSERT_EQ(ended.size(), 1u);
    EXPECT_LT(asked[0], ended[0]);
}

TEST(ClientEngineBounds, AFullQueueKeepsTheRoundsOfACall) {
    auto options = with_prior(only({newest}), Transport::stdio, newest);
    options.input.form = true;
    options.max_queued_events = 1;
    Rig rig(options);
    rig.engine->open();  // one contact change: the queue is full
    neomcp::OutgoingCall call;
    call.message.method = "tools/call";
    call.message.params = json{{"name", "delete"}, {"arguments", json::object()}};
    call.progress = true;
    const auto ref = rig.send(call);
    rig.engine->received(neomcp::connection_unit,
                         neomcp::serialize(neomcp::make_notification(
                             "notifications/progress", json{{"progressToken", ref.value}, {"progress", 1}})));
    rig.answer("tools/call", json::parse(R"({"resultType":"input_required","requestState":"s",
        "inputRequests":{"confirm":{"method":"elicitation/create","params":{"mode":"form","message":"Sure?",
        "requestedSchema":{"type":"object","properties":{"ok":{"type":"boolean"}}}}}}})"));
    EXPECT_EQ(rig.engine->dropped_events(), 1u);
    const auto rounds = rig.all<neomcp::InputRoundStarted>();
    ASSERT_EQ(rounds.size(), 1u);
    EXPECT_EQ(rounds[0].request, ref);
    EXPECT_TRUE(rig.all<neomcp::ProgressEvent>().empty());
}

std::string padded(std::string text, std::size_t size) {
    text.resize(size, ' ');
    return text;
}

TEST(ClientEngineBounds, AMessageAtTheSizeBoundIsReadAndOneByteMoreIsNot) {
    auto options = with_prior(only({newest}), Transport::stdio, newest);
    options.max_message_bytes = 96;
    Rig rig(options);
    rig.engine->open();
    const auto ref = rig.send("tools/list");
    const auto line = neomcp::serialize(neomcp::make_result(neomcp::RequestId(1), json{{"tools", json::array()}}));
    ASSERT_LT(line.size(), 96u);
    EXPECT_EQ(rig.engine->received(neomcp::connection_unit, padded(line, 97)), neomcp::Reception::too_large);
    EXPECT_EQ(rig.diagnostics(DiagnosticKind::too_large).size(), 1u);
    EXPECT_FALSE(rig.ended(ref));  // on stdio the line is dropped and the call waits on
    EXPECT_EQ(rig.engine->received(neomcp::connection_unit, padded(line, 96)), neomcp::Reception::accepted);
    EXPECT_TRUE(rig.ended(ref));
}

TEST(ClientEngineBounds, OnHttpAMessageTooLargeFailsTheCallOfItsExchange) {
    auto options = with_prior(only({newest}), Transport::streamable_http, newest);
    options.max_message_bytes = 64;
    Rig rig(options, Transport::streamable_http);
    rig.engine->open();
    const auto ref = rig.send("tools/list");
    const auto unit = rig.last_unit("tools/list");
    EXPECT_EQ(rig.engine->received(unit, std::string(65, ' ')), neomcp::Reception::too_large);
    EXPECT_EQ(failure_of(rig.ended(ref)), CallFailureKind::message_too_large);
    ASSERT_EQ(rig.outputs(OutgoingKind::end_unit).size(), 1u);
}

TEST(ClientEngineBounds, AMessageAtTheNestingBoundIsReadAndOneLevelMoreIsNot) {
    auto options = with_prior(only({newest}), Transport::stdio, newest);
    options.max_nesting_depth = 3;
    Rig rig(options);
    rig.engine->open();
    const auto ref = rig.send("tools/list");
    EXPECT_EQ(rig.engine->received(neomcp::connection_unit,
                                   R"({"jsonrpc":"2.0","id":1,"result":{"tools":[{"name":"x"}]}})"),
              neomcp::Reception::too_deep);
    EXPECT_EQ(rig.diagnostics(DiagnosticKind::too_deep).size(), 1u);
    EXPECT_FALSE(rig.ended(ref));
    EXPECT_EQ(rig.engine->received(neomcp::connection_unit, R"({"jsonrpc":"2.0","id":1,"result":{"tools":[]}})"),
              neomcp::Reception::accepted);
    EXPECT_TRUE(rig.ended(ref));
}

TEST(ClientEngineBounds, AProcessThatKeepsEndingFailsContactAfterMaxRestarts) {
    auto options = with_prior(only({newest}), Transport::stdio, newest);
    options.max_restarts = 2;
    Rig rig(options);
    rig.engine->open();
    for (int exit = 1; exit <= 2; ++exit) {
        rig.send("tools/list");
        rig.engine->stream_event(neomcp::connection_unit, neomcp::ProcessExited{exit});
        ASSERT_EQ(rig.engine->contact().phase, ContactPhase::lost) << exit;
        rig.send("tools/list");
        ASSERT_EQ(rig.outputs(OutgoingKind::restart).size(), static_cast<std::size_t>(exit));
        rig.engine->transport_restarted();
        ASSERT_EQ(rig.engine->contact().phase, ContactPhase::open);
    }
    rig.engine->stream_event(neomcp::connection_unit, neomcp::ProcessExited{3});
    EXPECT_EQ(rig.engine->contact().phase, ContactPhase::failed);
    EXPECT_EQ(rig.engine->contact().failure->kind, neomcp::ContactFailureKind::too_many_restarts);
}

TEST(ClientEngineBounds, AnAnswerBetweenExitsStartsTheCountAgain) {
    auto options = with_prior(only({newest}), Transport::stdio, newest);
    options.max_restarts = 1;
    Rig rig(options);
    rig.engine->open();
    for (int exit = 1; exit <= 3; ++exit) {
        rig.engine->stream_event(neomcp::connection_unit, neomcp::ProcessExited{exit});
        ASSERT_EQ(rig.engine->contact().phase, ContactPhase::lost) << exit;
        rig.send("tools/list");
        rig.engine->transport_restarted();
        rig.answer("tools/list", json{{"tools", json::array()}});
    }
    EXPECT_EQ(rig.engine->contact().phase, ContactPhase::open);
}

TEST(ClientEngineBounds, OptionsThatCannotWorkAreRefused) {
    auto zero = every_revision();
    zero.max_pending_requests = 0;
    zero.max_queued_events = 0;
    zero.request_timeout = std::chrono::milliseconds{0};
    auto made = neomcp::ClientEngine::create(zero, Transport::stdio, [](neomcp::Outgoing) {});
    ASSERT_TRUE(std::holds_alternative<neomcp::ClientMechanismsError>(made));
    const auto& message = std::get<neomcp::ClientMechanismsError>(made).message;
    EXPECT_NE(message.find("max_pending_requests"), std::string::npos);
    EXPECT_NE(message.find("max_queued_events"), std::string::npos);
    EXPECT_NE(message.find("request_timeout"), std::string::npos);
    auto empty_sink = neomcp::ClientEngine::create(every_revision(), Transport::stdio, nullptr);
    EXPECT_TRUE(std::holds_alternative<neomcp::ClientMechanismsError>(empty_sink));
    auto no_transport = neomcp::ClientEngine::create(only({"2024-11-05"}), Transport::streamable_http,
                                                     [](neomcp::Outgoing) {});
    EXPECT_TRUE(std::holds_alternative<neomcp::ClientMechanismsError>(no_transport));
}

// ---- the sink

TEST(ClientEngineSink, ASinkThatThrowsClosesTheEngine) {
    Rig rig(with_prior(only({newest}), Transport::stdio, newest));
    rig.engine->open();
    const auto first = rig.send("tools/list");
    bool fail = true;
    rig.on_output = [&fail](const neomcp::Outgoing&) {
        if (fail) throw std::runtime_error("the pipe is gone");
    };
    EXPECT_THROW(rig.send("prompts/list"), std::runtime_error);
    fail = false;
    EXPECT_EQ(rig.engine->pending(), 0u);
    EXPECT_EQ(failure_of(rig.ended(first)), CallFailureKind::closed);
    for (const auto& ended : rig.all<neomcp::RequestEnded>()) {
        EXPECT_EQ(std::get<neomcp::CallFailure>(ended.outcome).kind, CallFailureKind::closed);
    }
    const auto written = rig.out.size();
    const auto later = rig.engine->send(list_call());
    EXPECT_TRUE(std::holds_alternative<neomcp::CallFailure>(later));
    EXPECT_EQ(rig.engine->received(neomcp::connection_unit, "{}"), neomcp::Reception::ignored);
    rig.engine->close();
    EXPECT_EQ(rig.out.size(), written);
    EXPECT_EQ(rig.engine->contact().phase, ContactPhase::closed);
}

TEST(ClientEngineSink, ACallFromInsideTheSinkThrows) {
    Rig rig(with_prior(only({newest}), Transport::stdio, newest));
    rig.engine->open();
    bool reentered = false;
    rig.on_output = [&](const neomcp::Outgoing&) {
        if (reentered) return;
        reentered = true;
        rig.engine->cancel(RequestRef{1});
    };
    EXPECT_THROW(rig.send("tools/list"), std::logic_error);
    EXPECT_TRUE(reentered);
    EXPECT_EQ(rig.engine->pending(), 0u);
}

TEST(ClientEngineSink, CloseEndsEveryCallAndWritesClose) {
    Rig rig(with_prior(only({newest}), Transport::stdio, newest));
    rig.engine->open();
    const auto a = rig.send("tools/list");
    rig.engine->close();
    EXPECT_EQ(failure_of(rig.ended(a)), CallFailureKind::closed);
    EXPECT_EQ(rig.outputs(OutgoingKind::close).size(), 1u);
    EXPECT_EQ(rig.engine->contact().phase, ContactPhase::closed);
    EXPECT_EQ(rig.clock(a, DeadlineKind::wait)->action, DeadlineAction::stop);
}

// ---- malformed messages of the server

TEST(ClientEngineMalformed, NothingAServerSendsThrowsOrEndsACall) {
    for (const auto transport : {Transport::stdio, Transport::streamable_http}) {
        auto options = only({handshake_revision});
        options.input.roots = true;
        Rig rig(options, transport);
        open_handshake(rig, handshake_revision);
        const auto ref = rig.send("tools/list");
        const auto unit = transport == Transport::stdio ? neomcp::connection_unit : rig.last_unit("tools/list");
        const std::vector<std::string> lines = {
            "", "{", "[]", "[1,2]", "42", "null", "\"text\"", R"({"jsonrpc":"2.0"})", R"({"jsonrpc":"1.0","id":1})",
            R"({"jsonrpc":"2.0","id":null,"error":{"code":-32700,"message":"Parse error"}})",
            R"({"jsonrpc":"2.0","id":-5,"result":{}})", R"({"jsonrpc":"2.0","id":1.5,"result":{}})",
            R"({"jsonrpc":"2.0","id":18446744073709551615,"result":{}})",
            R"({"jsonrpc":"2.0","method":"notifications/progress","params":{"progressToken":[],"progress":"x"}})",
            R"({"jsonrpc":"2.0","method":"notifications/progress","params":{"progressToken":2,"progress":1}})",
            R"({"jsonrpc":"2.0","method":"notifications/message","params":{"level":"loud","data":1}})",
            R"({"jsonrpc":"2.0","method":"notifications/resources/updated","params":{}})",
            R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":{}}})",
            R"({"jsonrpc":"2.0","id":3,"method":"no/such/method"})",
            R"({"jsonrpc":"2.0","id":4,"method":"sampling/createMessage","params":{"messages":"x"}})",
            R"({"jsonrpc":"2.0","id":5,"method":"roots/list","params":[]})",
            "\xff\xfe\x00garbage", R"({"jsonrpc":"2.0","id":77,"error":{"code":"x"}})",
            R"([{"jsonrpc":"2.0","method":"x"}])"};
        for (const auto& line : lines) {
            EXPECT_NO_THROW(rig.engine->received(unit, line)) << line;
        }
        EXPECT_FALSE(rig.ended(ref));
        EXPECT_EQ(rig.engine->contact().phase, ContactPhase::open);
        EXPECT_FALSE(rig.diagnostics(DiagnosticKind::not_json).empty());
        EXPECT_FALSE(rig.diagnostics(DiagnosticKind::not_message).empty());
        EXPECT_FALSE(rig.diagnostics(DiagnosticKind::malformed_message).empty());
    }
}

TEST(ClientEngineMalformed, MutatedMessagesNeverThrow) {
    Random random(20261120);
    const std::vector<std::string> seeds = {
        R"({"jsonrpc":"2.0","id":2,"result":{"tools":[{"name":"x","inputSchema":{"type":"object"}}]}})",
        R"({"jsonrpc":"2.0","method":"notifications/progress","params":{"progressToken":2,"progress":0.5}})",
        R"({"jsonrpc":"2.0","id":"s","method":"roots/list"})",
        R"({"jsonrpc":"2.0","method":"notifications/message","params":{"level":"info","data":{"a":[1,2]}}})",
        R"({"jsonrpc":"2.0","id":2,"error":{"code":-32022,"message":"m","data":{"supported":["x"]}}})"};
    const std::string alphabet = "{}[]\",:0123456789-.eE truefalsnul\\u\xc3\xa9\x01";
    auto options = only({handshake_revision});
    options.input.roots = true;
    Rig rig(options);
    open_handshake(rig, handshake_revision);
    for (int round = 0; round < 3000; ++round) {
        std::string line = seeds[random.below(seeds.size())];
        const std::size_t edits = 1 + random.below(4);
        for (std::size_t edit = 0; edit < edits && !line.empty(); ++edit) {
            const std::size_t at = random.below(line.size());
            switch (random.below(3)) {
                case 0: line[at] = alphabet[random.below(alphabet.size())]; break;
                case 1: line.erase(at, 1); break;
                default: line.insert(at, 1, alphabet[random.below(alphabet.size())]); break;
            }
        }
        if (round % 50 == 0) rig.send("tools/list");
        ASSERT_NO_THROW(rig.engine->received(neomcp::connection_unit, line)) << line;
        rig.drain();
        rig.log.clear();
    }
}

}  // namespace
