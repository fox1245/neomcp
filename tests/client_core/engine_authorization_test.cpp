// What the engine makes of a request its transport holds for authorization: the clocks of the calls on the unit wait
// for the hold's end (the probe's as well), a timer set before the hold is ignored, and the authorizer's failure ends
// the call, or fails contact when it ends the probe, with what the authorizer met.
#include <gtest/gtest.h>

#include <string_view>

#include "engine_rig.h"
#include "neomcp/client/engine.h"
#include "neomcp/profile.h"

namespace {

using client_core_test::discover_result;
using client_core_test::only;
using client_core_test::Rig;
using neomcp::AuthorizationDenied;
using neomcp::AuthorizationFailure;
using neomcp::AuthorizationHold;
using neomcp::AuthorizationProblem;
using neomcp::CallFailure;
using neomcp::CallFailureKind;
using neomcp::ContactFailureKind;
using neomcp::ContactPhase;
using neomcp::DeadlineAction;
using neomcp::DeadlineKind;
using neomcp::json;
using neomcp::profile::Transport;

constexpr std::string_view modern = "2026-07-28";

CallFailure failed(AuthorizationProblem problem) {
    return CallFailure{CallFailureKind::authorization_failed, "the sign-in did not complete",
                       AuthorizationFailure{problem, "https://issuer.test", "", std::nullopt}};
}

std::unique_ptr<Rig> opened() {
    auto rig = std::make_unique<Rig>(only({modern}), Transport::streamable_http);
    rig->engine->open();
    rig->answer("server/discover", discover_result({modern}));
    rig->engine->stream_event(rig->last_unit("server/discover"), neomcp::StreamEnded{});
    return rig;
}

TEST(ClientEngineAuthorization, AHoldPausesTheClocksOfTheCallUntilItEnds) {
    auto rig = opened();
    ASSERT_EQ(rig->engine->contact().phase, ContactPhase::open);
    const auto ref = rig->send("tools/list");
    const auto unit = rig->last_unit("tools/list");
    const auto started = rig->clock(ref, DeadlineKind::wait);
    ASSERT_TRUE(started);
    EXPECT_EQ(started->action, DeadlineAction::start);

    rig->engine->stream_event(unit, AuthorizationHold{true});
    const auto paused = rig->clock(ref, DeadlineKind::wait);
    ASSERT_TRUE(paused);
    EXPECT_EQ(paused->action, DeadlineAction::pause);
    // The timer the driver set before the hold lapses: the call does not end.
    rig->engine->expire(started->deadline);
    EXPECT_FALSE(rig->ended(ref));

    rig->engine->stream_event(unit, AuthorizationHold{false});
    const auto resumed = rig->clock(ref, DeadlineKind::wait);
    ASSERT_TRUE(resumed);
    EXPECT_EQ(resumed->action, DeadlineAction::resume);
    rig->answer("tools/list", json{{"tools", json::array()}});
    const auto ended = rig->ended(ref);
    ASSERT_TRUE(ended);
    EXPECT_TRUE(std::holds_alternative<neomcp::Answered>(ended->outcome));
}

TEST(ClientEngineAuthorization, AHoldOfTheProbeKeepsItFromTimingOut) {
    auto rig = std::make_unique<Rig>(only({modern}), Transport::streamable_http);
    rig->engine->open();
    const auto unit = rig->last_unit("server/discover");
    const auto changes = rig->all<neomcp::DeadlineChange>();
    ASSERT_FALSE(changes.empty());
    const auto started = changes.back();
    rig->engine->stream_event(unit, AuthorizationHold{true});
    rig->engine->expire(started.deadline);
    EXPECT_EQ(rig->engine->contact().phase, ContactPhase::probing);
    rig->engine->stream_event(unit, AuthorizationHold{false});
    rig->answer("server/discover", discover_result({modern}));
    EXPECT_EQ(rig->engine->contact().phase, ContactPhase::open);
}

TEST(ClientEngineAuthorization, ADenialEndsTheCallWithTheAuthorizersFailure) {
    auto rig = opened();
    const auto ref = rig->send("tools/list");
    const auto unit = rig->last_unit("tools/list");
    rig->engine->stream_event(unit, AuthorizationHold{true});
    rig->engine->stream_event(unit, AuthorizationDenied{failed(AuthorizationProblem::state_mismatch), 401});
    const auto ended = rig->ended(ref);
    ASSERT_TRUE(ended);
    const auto* failure = std::get_if<CallFailure>(&ended->outcome);
    ASSERT_NE(failure, nullptr);
    EXPECT_EQ(failure->kind, CallFailureKind::authorization_failed);
    ASSERT_TRUE(failure->authorization);
    EXPECT_EQ(failure->authorization->problem, AuthorizationProblem::state_mismatch);
    // The exchange was ended.
    bool ended_unit = false;
    for (const auto& outgoing : rig->outputs(neomcp::OutgoingKind::end_unit)) ended_unit = ended_unit || outgoing.unit == unit;
    EXPECT_TRUE(ended_unit);
    EXPECT_EQ(rig->engine->contact().phase, ContactPhase::open);
}

TEST(ClientEngineAuthorization, ADenialOfTheProbeFailsContactWithTheAuthorizersFailure) {
    auto rig = std::make_unique<Rig>(only({modern}), Transport::streamable_http);
    rig->engine->open();
    const auto unit = rig->last_unit("server/discover");
    rig->engine->stream_event(unit, AuthorizationDenied{failed(AuthorizationProblem::discovery_failed), 401});
    const auto& contact = rig->engine->contact();
    EXPECT_EQ(contact.phase, ContactPhase::failed);
    ASSERT_TRUE(contact.failure);
    EXPECT_EQ(contact.failure->kind, ContactFailureKind::authorization_failed);
    ASSERT_TRUE(contact.failure->authorization);
    EXPECT_EQ(contact.failure->authorization->problem, AuthorizationProblem::discovery_failed);
}

}  // namespace
