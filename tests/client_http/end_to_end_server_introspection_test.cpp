// The first contact, and a step-up for a tool's scope, against the library's own protected server when it asks the
// authorization server about each token (the introspection validator, RFC 7662). The scripted authorization server
// answers the introspection requests of the resource server.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "end_to_end_server_rig.h"
#include "neomcp/base64.h"
#include "neomcp/http/introspection.h"

namespace {

using namespace server_rig;
using oauth_rig::grants_of;

constexpr std::string_view introspection_path = "/introspect";
constexpr std::string_view introspection_id = "resource-server";
constexpr std::string_view introspection_secret = "resource-secret";

class IntrospectionEvents {
public:
    void add(neomcp::http::IntrospectionEvent event) {
        std::lock_guard lock(mutex_);
        events_.push_back(event);
    }
    std::size_t count(neomcp::http::IntrospectionEvent event) const {
        std::lock_guard lock(mutex_);
        return static_cast<std::size_t>(std::count(events_.begin(), events_.end(), event));
    }
    std::size_t total() const {
        std::lock_guard lock(mutex_);
        return events_.size();
    }

private:
    mutable std::mutex mutex_;
    std::vector<neomcp::http::IntrospectionEvent> events_;
};

// The authorization server answers an introspection request of the resource server as RFC 7662 describes: the token's
// grant while the ledger holds it, `active: false` for any other.
void serve_introspection(ServerWorld& world) {
    const std::string expected = "Basic " + neomcp::base64_encode(std::string(introspection_id) + ":" +
                                                                   std::string(introspection_secret));
    world.as.serve_extra([&world, expected](const oauth_rig::Request& request) -> std::optional<http_test::Reply> {
        if (request.method != "POST" || request.target != introspection_path) return std::nullopt;
        if (request.field("Authorization") != expected) {
            return http_test::json_reply(401, json{{"error", "invalid_client"}});
        }
        const auto token = field_of(request.body, "token");
        const auto scopes = token ? world.ledger.scopes_of(*token) : std::nullopt;
        const auto grant = token ? world.as.grant_of(*token) : std::nullopt;
        if (!scopes || !grant) return http_test::json_reply(200, json{{"active", false}});
        json answer{{"active", true},
                    {"iss", world.as.issuer()},
                    {"sub", grant->subject},
                    {"client_id", grant->client_id},
                    {"aud", grant->audience},
                    {"exp", epoch_seconds(grant->expires)}};
        if (!scopes->empty()) answer["scope"] = join_scopes(*scopes);
        return http_test::json_reply(200, answer);
    });
}

ServerSetup introspection_setup(const std::shared_ptr<IntrospectionEvents>& events) {
    ServerSetup setup;
    setup.validator = [events](ServerWorld& world) {
        neomcp::http::IntrospectionOptions options;
        options.endpoint = world.as.server().url(introspection_path);
        options.issuer = world.as.issuer();
        options.authentication =
            neomcp::http::IntrospectionBasicAuth{std::string(introspection_id), std::string(introspection_secret)};
        options.bounds.request_timeout = 10s;
        options.bounds.connect_timeout = 5s;
        options.observer = [events](neomcp::http::IntrospectionEvent event) { events->add(event); };
        auto made = neomcp::http::make_introspection_validator(std::move(options));
        if (!made) throw std::runtime_error("the validator was refused: " + made.error);
        return *made.value;
    };
    return setup;
}

class ServerEndToEndIntrospection : public ::testing::TestWithParam<std::string> {};

TEST_P(ServerEndToEndIntrospection, SignsInAndTheServerAsksTheAuthorizationServerAboutEachToken) {
    auto events = std::make_shared<IntrospectionEvents>();
    ServerWorld world(introspection_setup(events));
    serve_introspection(world);
    expect_first_contact_and_step_up(world, GetParam());

    // Each of the two tokens was asked about once: the verdict was remembered after that. The request names the token
    // in its body (never in its target), as the resource server, with the credentials it was configured with.
    const auto asked = world.as.requests_to(std::string(introspection_path));
    ASSERT_EQ(asked.size(), 2u);
    EXPECT_EQ(events->count(neomcp::http::IntrospectionEvent::active), 2u);
    EXPECT_EQ(events->total(), 2u);
    std::vector<std::string> named;
    for (const auto& one : asked) {
        EXPECT_EQ(field_of(one.body, "token_type_hint"), "access_token");
        if (const auto token = field_of(one.body, "token")) named.push_back(*token);
    }
    std::vector<std::string> issued = world.as.access_tokens();
    std::sort(named.begin(), named.end());
    std::sort(issued.begin(), issued.end());
    EXPECT_EQ(named, issued);
}

// A token the authorization server no longer holds is refused, because the server does not remember verdicts here: the
// client refreshes it without the person and goes on.
TEST_P(ServerEndToEndIntrospection, RefusesATokenTheAuthorizationServerNoLongerHoldsAndTheClientRefreshes) {
    auto events = std::make_shared<IntrospectionEvents>();
    ServerSetup setup = introspection_setup(events);
    setup.authorization = [](neomcp::http::ResourceServerOptions& options) {
        options.bounds.max_cache_age = 0s;
        options.bounds.negative_cache_age = 0s;
    };
    ServerWorld world(std::move(setup));
    serve_introspection(world);
    use_revision(world, GetParam());
    world.as.issue_refresh_tokens(true);
    auto client = world.connect();
    ASSERT_TRUE(client) << client.error().message;
    const auto before = client->call_tool("open");
    ASSERT_TRUE(before) << before.error().message;
    ASSERT_EQ(world.as.access_tokens().size(), 1u);
    const std::string old_token = world.as.access_tokens().front();

    world.ledger.expire_all();
    const auto after = client->call_tool("open");
    ASSERT_TRUE(after) << after.error().message;
    EXPECT_EQ(text_of(*after), "open");
    EXPECT_EQ(world.person.prompts().size(), 1u);
    EXPECT_EQ(grants_of(world.as, "refresh_token"), 1u);
    ASSERT_EQ(world.as.access_tokens().size(), 2u);
    const std::vector<int> old_statuses = statuses_for(world, old_token);
    ASSERT_FALSE(old_statuses.empty());
    EXPECT_EQ(old_statuses.back(), 401);
    EXPECT_GE(events->count(neomcp::http::IntrospectionEvent::inactive), 1u);
    EXPECT_EQ(last_authorized_status(world), 200);
    expect_tokens_kept(world);
    EXPECT_TRUE(client->close());
}

INSTANTIATE_TEST_SUITE_P(ServerEndToEndIntrospectionRevisions, ServerEndToEndIntrospection,
                         ::testing::Values("2025-03-26", "2025-06-18", "2025-11-25", "2026-07-28", "all"),
                         [](const auto& info) { return revision_name(info.param); });


}  // namespace
