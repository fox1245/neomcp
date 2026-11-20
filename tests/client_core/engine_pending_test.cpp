// The pending table of the client engine: request ids, and which answers count.
#include <gtest/gtest.h>

#include "engine_rig.h"

namespace {

using namespace client_core_test;
using neomcp::DiagnosticKind;
using neomcp::OutgoingKind;
using neomcp::profile::Transport;

// A client that speaks only revisions that name themselves per request, told up front which kind of server answers.
neomcp::EngineOptions per_request_with_prior(Transport transport) {
    auto options = only({"2026-07-28"});
    options.prior = neomcp::ContactVerdict{neomcp::ServerKind::per_request, transport, "2026-07-28"};
    return options;
}

void open_with_handshake(Rig& rig, std::string_view revision) {
    rig.engine->open();
    ASSERT_EQ(rig.answer("initialize", initialize_result(revision)), neomcp::Reception::accepted);
    ASSERT_EQ(rig.engine->contact().phase, neomcp::ContactPhase::open);
}

TEST(ClientEnginePending, IdsAreIntegersFromOneInTheOrderSent) {
    Rig rig(only({"2025-11-25"}));
    open_with_handshake(rig, "2025-11-25");
    rig.send("tools/list");
    rig.send("prompts/list");
    const auto requests = rig.requests();
    ASSERT_EQ(requests.size(), 3u);
    for (std::size_t i = 0; i < requests.size(); ++i) EXPECT_EQ(requests[i]["id"], json(i + 1)) << i;
    EXPECT_EQ(requests[0]["method"], "initialize");
}

TEST(ClientEnginePending, AnAnswerWithAnUnknownIdIsDroppedAsADiagnostic) {
    Rig rig(per_request_with_prior(Transport::stdio));
    rig.engine->open();
    const auto ref = rig.send("tools/list");
    EXPECT_EQ(rig.engine->received(neomcp::connection_unit,
                                   neomcp::serialize(neomcp::make_result(neomcp::RequestId(99), json::object()))),
              neomcp::Reception::accepted);
    EXPECT_EQ(rig.diagnostics(DiagnosticKind::unknown_id).size(), 1u);
    EXPECT_FALSE(rig.ended(ref));
    // A string id is no id this client gives.
    rig.engine->received(neomcp::connection_unit,
                         neomcp::serialize(neomcp::make_result(neomcp::RequestId("1"), json::object())));
    EXPECT_EQ(rig.diagnostics(DiagnosticKind::unknown_id).size(), 2u);
    EXPECT_FALSE(rig.ended(ref));
    EXPECT_EQ(rig.engine->pending(), 1u);
}

TEST(ClientEnginePending, ASecondAnswerIsDroppedAsRepeated) {
    Rig rig(per_request_with_prior(Transport::stdio));
    rig.engine->open();
    const auto ref = rig.send("tools/list");
    rig.answer("tools/list", json{{"tools", json::array()}});
    const auto first = rig.ended(ref);
    ASSERT_TRUE(first);
    EXPECT_TRUE(std::holds_alternative<neomcp::Answered>(first->outcome));
    rig.answer("tools/list", json{{"tools", json::array()}});
    EXPECT_EQ(rig.diagnostics(DiagnosticKind::repeated_answer).size(), 1u);
    EXPECT_EQ(rig.all<neomcp::RequestEnded>().size(), 1u);
    EXPECT_EQ(rig.engine->pending(), 0u);
}

TEST(ClientEnginePending, AnAnswerOnAnotherUnitIsDroppedAndTheCallStaysPending) {
    Rig rig(per_request_with_prior(Transport::streamable_http), Transport::streamable_http);
    rig.engine->open();
    const auto first = rig.send("tools/list");
    const auto first_id = rig.last_id("tools/list");
    const auto first_unit = rig.last_unit("tools/list");
    const auto second = rig.send("prompts/list");
    const auto second_unit = rig.last_unit("prompts/list");
    ASSERT_NE(first_unit, second_unit);
    ASSERT_NE(first_unit, neomcp::connection_unit);
    const auto line =
        neomcp::serialize(neomcp::make_result(neomcp::RequestId(first_id), json{{"tools", json::array()}}));
    rig.engine->received(second_unit, line);
    EXPECT_EQ(rig.diagnostics(DiagnosticKind::wrong_unit).size(), 1u);
    EXPECT_FALSE(rig.ended(first));
    rig.engine->received(neomcp::connection_unit, line);
    EXPECT_EQ(rig.diagnostics(DiagnosticKind::wrong_unit).size(), 2u);
    EXPECT_FALSE(rig.ended(first));
    rig.engine->received(first_unit, line);
    ASSERT_TRUE(rig.ended(first));
    EXPECT_TRUE(std::holds_alternative<neomcp::Answered>(rig.ended(first)->outcome));
    EXPECT_FALSE(rig.ended(second));
}

TEST(ClientEnginePending, IdsAreNotUsedAgainAfterTheServerRestarted) {
    Rig rig(per_request_with_prior(Transport::stdio));
    rig.engine->open();
    const auto before = rig.send("tools/list");
    EXPECT_EQ(rig.last_id("tools/list"), 1u);
    rig.engine->stream_event(neomcp::connection_unit, neomcp::ProcessExited{1});
    const auto lost = rig.ended(before);
    ASSERT_TRUE(lost);
    ASSERT_TRUE(std::holds_alternative<neomcp::CallFailure>(lost->outcome));
    EXPECT_EQ(std::get<neomcp::CallFailure>(lost->outcome).kind, neomcp::CallFailureKind::server_exited);

    const auto after = rig.send("tools/list");
    EXPECT_EQ(rig.outputs(OutgoingKind::restart).size(), 1u);
    EXPECT_EQ(rig.requests().size(), 1u);  // held until the new process runs
    rig.engine->transport_restarted();
    ASSERT_EQ(rig.requests().size(), 2u);
    EXPECT_EQ(rig.last_id("tools/list"), 2u);
    // An answer to the request of the old process is no answer to the new one.
    rig.engine->received(neomcp::connection_unit,
                         neomcp::serialize(neomcp::make_result(neomcp::RequestId(1), json{{"tools", json::array()}})));
    EXPECT_FALSE(rig.ended(after));
    rig.answer("tools/list", json{{"tools", json::array()}});
    EXPECT_TRUE(rig.ended(after));
}

TEST(ClientEnginePending, ALateAnswerAfterExpiryIsAnUnknownId) {
    Rig rig(per_request_with_prior(Transport::stdio));
    rig.engine->open();
    const auto ref = rig.send("tools/list");
    const auto wait = rig.clock(ref, neomcp::DeadlineKind::wait);
    ASSERT_TRUE(wait);
    rig.engine->expire(wait->deadline);
    ASSERT_TRUE(rig.ended(ref));
    EXPECT_EQ(std::get<neomcp::CallFailure>(rig.ended(ref)->outcome).kind, neomcp::CallFailureKind::timed_out);
    rig.answer("tools/list", json{{"tools", json::array()}});
    EXPECT_EQ(rig.diagnostics(DiagnosticKind::unknown_id).size(), 1u);
    EXPECT_EQ(rig.all<neomcp::RequestEnded>().size(), 1u);
}

}  // namespace
