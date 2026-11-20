// The probe of which kind of server answers, over Streamable HTTP, where the status of an answer decides together with
// its body: a 404 whose body is Method not found, an error with a status after which no step goes on, a handshake
// met by an error only a server that names revisions per request sends, and a handshake refused by its status, whose
// outcome names the older transport of events over HTTP as the next step.
#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "engine_rig.h"
#include "neomcp/client/engine.h"
#include "neomcp/jsonrpc.h"
#include "neomcp/profile.h"

namespace {

using client_core_test::discover_result;
using client_core_test::initialize_result;
using client_core_test::only;
using client_core_test::Rig;
using neomcp::ContactFailureKind;
using neomcp::ContactPhase;
using neomcp::json;
using neomcp::ServerKind;
using neomcp::profile::Transport;

constexpr std::string_view modern = "2026-07-28";
constexpr std::string_view legacy = "2025-11-25";
constexpr std::string_view older_transport = "2024-11-05";
constexpr std::string_view json_media = "application/json";

std::size_t count(const Rig& rig, std::string_view method) {
    std::size_t found = 0;
    for (const auto& request : rig.requests()) {
        if (request["method"] == std::string(method)) ++found;
    }
    return found;
}

// The head of the answer to the last request of `method`, as a transport delivers it before the body.
void head(Rig& rig, std::string_view method, int status, std::optional<std::string_view> media = json_media) {
    std::optional<std::string> type;
    if (media) type = std::string(*media);
    rig.engine->stream_event(rig.last_unit(method), neomcp::ResponseHead{status, type, {}});
}

void end(Rig& rig, std::string_view method) { rig.engine->stream_event(rig.last_unit(method), neomcp::StreamEnded{}); }

// An error answer with a status, then the end of its exchange.
void refuse_with(Rig& rig, std::string_view method, int status, int code) {
    head(rig, method, status);
    rig.refuse(method, code, "refused");
    end(rig, method);
}

// A status without a body.
void status_only(Rig& rig, std::string_view method, int status) {
    head(rig, method, status, std::nullopt);
    end(rig, method);
}

std::unique_ptr<Rig> probing(neomcp::EngineOptions options) {
    auto rig = std::make_unique<Rig>(std::move(options), Transport::streamable_http);
    rig->engine->open();
    return rig;
}

TEST(ClientEngineHttpProbe, ANotFoundWithMethodNotFoundIsAModernServer) {
    auto rig = probing(only({modern, legacy}));
    refuse_with(*rig, "server/discover", 404, neomcp::error_code::method_not_found);
    ASSERT_EQ(rig->engine->contact().phase, ContactPhase::open);
    EXPECT_EQ(rig->engine->contact().verdict->kind, ServerKind::per_request);
    EXPECT_EQ(rig->engine->contact().verdict->revision, modern);
    EXPECT_EQ(count(*rig, "initialize"), 0u);
}

TEST(ClientEngineHttpProbe, MethodNotFoundWithAnotherStatusIsNoModernServer) {
    // In a success: a server that handshakes, as for any other error there.
    auto success = probing(only({modern, legacy}));
    refuse_with(*success, "server/discover", 200, neomcp::error_code::method_not_found);
    EXPECT_EQ(success->engine->contact().phase, ContactPhase::handshaking);
    EXPECT_EQ(count(*success, "initialize"), 1u);
    // With 400: not the status a modern server gives an unknown method; the step that follows a refusing status.
    auto refusing = probing(only({modern, legacy}));
    refuse_with(*refusing, "server/discover", 400, neomcp::error_code::method_not_found);
    EXPECT_EQ(refusing->engine->contact().phase, ContactPhase::handshaking);
}

TEST(ClientEngineHttpProbe, AnErrorWithAStatusAfterWhichNoStepGoesOnFailsContact) {
    for (const int status : {500, 503, 409}) {
        auto rig = probing(only({modern, legacy}));
        refuse_with(*rig, "server/discover", status, neomcp::error_code::internal_error);
        ASSERT_EQ(rig->engine->contact().phase, ContactPhase::failed) << status;
        EXPECT_EQ(rig->engine->contact().failure->kind, ContactFailureKind::http_status) << status;
        EXPECT_EQ(count(*rig, "initialize"), 0u) << status;
    }
}

TEST(ClientEngineHttpProbe, AnErrorWithARefusingStatusOrASuccessFallsBackToTheHandshake) {
    for (const int status : {200, 400, 404, 405}) {
        auto rig = probing(only({modern, legacy}));
        refuse_with(*rig, "server/discover", status, neomcp::error_code::invalid_request);
        EXPECT_EQ(rig->engine->contact().phase, ContactPhase::handshaking) << status;
    }
}

TEST(ClientEngineHttpProbe, AnUnsupportedRevisionInAnyStatusIsAskedAgain) {
    auto rig = probing(only({modern, legacy}));
    const auto first = rig->last_unit("server/discover");
    head(*rig, "server/discover", 400);
    rig->refuse("server/discover", -32022, "unsupported", json{{"supported", json::array({std::string(modern)})}});
    rig->engine->stream_event(first, neomcp::StreamEnded{});
    EXPECT_EQ(rig->engine->contact().phase, ContactPhase::probing);
    EXPECT_EQ(count(*rig, "server/discover"), 2u);
}

TEST(ClientEngineHttpProbe, AHandshakeMetByAModernErrorProbesOnceMore) {
    auto rig = probing(only({modern, legacy}));
    status_only(*rig, "server/discover", 404);
    ASSERT_EQ(rig->engine->contact().phase, ContactPhase::handshaking);
    refuse_with(*rig, "initialize", 400, -32021);
    ASSERT_EQ(rig->engine->contact().phase, ContactPhase::probing);
    EXPECT_EQ(count(*rig, "server/discover"), 2u);
    head(*rig, "server/discover", 200);
    rig->answer("server/discover", discover_result({modern}));
    end(*rig, "server/discover");
    ASSERT_EQ(rig->engine->contact().phase, ContactPhase::open);
    EXPECT_EQ(rig->engine->contact().verdict->kind, ServerKind::per_request);
}

TEST(ClientEngineHttpProbe, AHandshakeIsProbedAgainOnlyOnce) {
    auto rig = probing(only({modern, legacy}));
    status_only(*rig, "server/discover", 404);
    refuse_with(*rig, "initialize", 400, -32021);
    head(*rig, "server/discover", 200);
    rig->answer("server/discover", json::object());  // no discovery result: the handshake once more
    end(*rig, "server/discover");
    ASSERT_EQ(count(*rig, "initialize"), 2u);
    refuse_with(*rig, "initialize", 500, -32021);
    EXPECT_EQ(rig->engine->contact().phase, ContactPhase::failed);
    EXPECT_EQ(count(*rig, "server/discover"), 2u);
}

TEST(ClientEngineHttpProbe, AHandshakeRefusedByItsStatusNamesTheOlderTransport) {
    for (const int status : {400, 404, 405}) {
        auto bare = probing(only({modern, legacy, older_transport}));
        status_only(*bare, "server/discover", 404);
        status_only(*bare, "initialize", status);
        ASSERT_EQ(bare->engine->contact().phase, ContactPhase::failed) << status;
        EXPECT_EQ(bare->engine->contact().failure->kind, ContactFailureKind::http_status) << status;
        EXPECT_EQ(bare->engine->contact().failure->next_transport, Transport::http_sse) << status;

        // A body that is no answer of a modern server changes nothing.
        auto with_body = probing(only({modern, legacy, older_transport}));
        status_only(*with_body, "server/discover", 404);
        refuse_with(*with_body, "initialize", status, neomcp::error_code::invalid_request);
        ASSERT_EQ(with_body->engine->contact().phase, ContactPhase::failed) << status;
        EXPECT_EQ(with_body->engine->contact().failure->next_transport, Transport::http_sse) << status;
    }
}

TEST(ClientEngineHttpProbe, WithoutTheOlderTransportEnabledARefusedHandshakeNamesNoNextStep) {
    auto rig = probing(only({modern, legacy}));
    status_only(*rig, "server/discover", 404);
    status_only(*rig, "initialize", 404);
    ASSERT_EQ(rig->engine->contact().phase, ContactPhase::failed);
    EXPECT_EQ(rig->engine->contact().failure->kind, ContactFailureKind::http_status);
    EXPECT_FALSE(rig->engine->contact().failure->next_transport);
}

TEST(ClientEngineHttpProbe, AHandshakeRefusedOtherwiseNamesNoNextStep) {
    auto server_error = probing(only({modern, legacy, older_transport}));
    status_only(*server_error, "server/discover", 404);
    status_only(*server_error, "initialize", 500);
    EXPECT_EQ(server_error->engine->contact().failure->kind, ContactFailureKind::http_status);
    EXPECT_FALSE(server_error->engine->contact().failure->next_transport);

    auto refused = probing(only({modern, legacy, older_transport}));
    status_only(*refused, "server/discover", 404);
    refuse_with(*refused, "initialize", 200, neomcp::error_code::invalid_request);
    EXPECT_EQ(refused->engine->contact().failure->kind, ContactFailureKind::handshake_refused);
    EXPECT_FALSE(refused->engine->contact().failure->next_transport);
}

TEST(ClientEngineHttpProbe, OnStdioMethodNotFoundIsAServerThatHandshakes) {
    Rig rig(only({modern, legacy}), Transport::stdio);
    rig.engine->open();
    rig.refuse("server/discover", neomcp::error_code::method_not_found, "unknown");
    EXPECT_EQ(rig.engine->contact().phase, ContactPhase::handshaking);
    rig.answer("initialize", initialize_result(legacy));
    EXPECT_EQ(rig.engine->contact().verdict->kind, ServerKind::handshake);
}

}  // namespace
