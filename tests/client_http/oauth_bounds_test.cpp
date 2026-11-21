// The bounds of a sign-in over loopback, through the public interface, where the bound is on a network answer or on
// how often something is repeated: a registration answer, the redirects of a metadata request, the parameters of a
// challenge, the authorization servers a document names, the scopes of a challenge, the time of a token or
// registration request and of the person, how often one request is held and how often one operation signs in again,
// and the options that cannot be zero. Each is tried one under, at and one over.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "oauth_rig.h"

namespace {

using namespace oauth_rig;
using neomcp::AuthorizationProblem;
using neomcp::ClientErrorKind;
using BoundsClock = std::chrono::steady_clock;

// Pads `document` with one more member so that its text is exactly `size` bytes.
void fill_to(json& document, std::size_t size) {
    document["pad"] = "";
    const std::size_t base = document.dump().size();
    ASSERT_LE(base, size);
    document["pad"] = std::string(size - base, 'a');
}

// The client opened (no problem), or failed with this sign-in problem.
void expect_signed_in_or(neomcp::Result<neomcp::Client>& client, std::optional<AuthorizationProblem> problem) {
    if (!problem) {
        ASSERT_TRUE(client) << client.error().message;
        EXPECT_TRUE(client->close());
        return;
    }
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().kind, ClientErrorKind::authorization_failed) << client.error().message;
    ASSERT_TRUE(client.error().authorization);
    EXPECT_EQ(client.error().authorization->problem, *problem);
}

// A client opened with the endpoint changed by `tweak`.
neomcp::Result<neomcp::Client> open_with(World& world, std::shared_ptr<neomcp::HttpAuthorizer> authorizer,
                                         const std::function<void(neomcp::HttpEndpoint&)>& tweak) {
    auto described = world.endpoint(std::move(authorizer));
    tweak(described);
    return neomcp::Client::open(world.client_options(), std::move(described));
}

std::size_t mcp_posts(const World& world) {
    std::size_t count = 0;
    for (const Request& one : world.resource.requests()) {
        if (one.method == "POST" && one.target == "/mcp") ++count;
    }
    return count;
}

// ---- A registration answer: max_token_response_bytes

TEST(OAuthBoundsHttp, ARegistrationAnswerAtItsBoundIsReadAndOneByteMoreIsNot) {
    constexpr std::size_t bound = 1536;
    for (const std::size_t size : {bound - 1, bound, bound + 1}) {
        SCOPED_TRACE(size);
        World world;
        world.as.override_with([size](const Request& request) -> std::optional<http_test::Reply> {
            if (request.target != "/register") return std::nullopt;
            json body{{"client_id", "padded-client"}, {"redirect_uris", json::array({std::string(redirect_uri)})}};
            fill_to(body, size);
            return http_test::json_reply(201, body);
        });
        neomcp::OAuthOptions options = world.options();
        options.bounds.max_token_response_bytes = bound;
        auto client = world.open(world.authorizer(std::move(options)));
        expect_signed_in_or(client, size > bound ? std::optional<AuthorizationProblem>(AuthorizationProblem::document_too_large)
                                                  : std::nullopt);
        if (size > bound) {
            EXPECT_EQ(world.person.prompts().size(), 0u);
        }
    }
}

// ---- Redirects of a metadata request: HttpEndpoint::max_redirects

// Serves `document` at `first` after `hops` redirects to other places of the same server.
Override redirect_chain(std::string first, std::size_t hops, json document) {
    return [first = std::move(first), hops, document = std::move(document)](
               const Request& request) -> std::optional<http_test::Reply> {
        if (request.method != "GET") return std::nullopt;
        const auto hop = [](std::size_t index) { return "/hop" + std::to_string(index); };
        std::size_t at = 0;
        if (request.target != first) {
            for (std::size_t index = 1; index <= hops; ++index) {
                if (request.target == hop(index)) at = index;
            }
            if (at == 0) return std::nullopt;
        }
        if (at == hops) return http_test::json_reply(200, document);
        return http_test::empty_reply(307, {{"Location", hop(at + 1)}});
    };
}

TEST(OAuthBoundsHttp, AMetadataRequestFollowsAsManyRedirectsAsTheEndpointAllowsAndNoMore) {
    struct Setting {
        std::optional<std::size_t> allowed;  // empty: the endpoint's default
        std::size_t effective;
    };
    for (const Setting setting : {Setting{std::nullopt, 5}, Setting{2, 2}, Setting{0, 0}}) {
        for (const bool resource_document : {true, false}) {
            for (const std::ptrdiff_t shift : {-1, 0, 1}) {
                const std::ptrdiff_t hops = static_cast<std::ptrdiff_t>(setting.effective) + shift;
                if (hops < 0) continue;
                SCOPED_TRACE(std::string(resource_document ? "resource " : "server ") + std::to_string(setting.effective) +
                             " allowed, " + std::to_string(hops) + " hops");
                World world;
                if (resource_document) {
                    const json document{{"resource", world.resource.url()},
                                        {"authorization_servers", json::array({world.as.issuer()})}};
                    world.resource.override_with(
                        redirect_chain("/.well-known/oauth-protected-resource/mcp", static_cast<std::size_t>(hops), document));
                } else {
                    world.as.override_with(redirect_chain("/.well-known/oauth-authorization-server",
                                                          static_cast<std::size_t>(hops), world.as.metadata_document()));
                }
                auto client = open_with(world, world.authorizer(), [&setting](neomcp::HttpEndpoint& described) {
                    if (setting.allowed) described.max_redirects = *setting.allowed;
                });
                expect_signed_in_or(client, shift > 0 ? std::optional<AuthorizationProblem>(AuthorizationProblem::discovery_failed)
                                                       : std::nullopt);
            }
        }
    }
}

// ---- Parameters of a challenge: max_challenge_params

// A 401 whose challenge names `custom` as its metadata and carries `params` parameters in all; the document is served
// there, and the well-known place serves it as well.
void challenge_with_params(World& world, std::size_t params) {
    const std::string custom = world.resource.server().url("/custom-prm");
    const json document{{"resource", world.resource.url()}, {"authorization_servers", json::array({world.as.issuer()})}};
    world.resource.override_with([custom, params, document](const Request& request) -> std::optional<http_test::Reply> {
        if (request.target == "/custom-prm") return http_test::json_reply(200, document);
        if (request.target != "/mcp" || request.field("Authorization")) return std::nullopt;
        std::string challenge = "Bearer resource_metadata=\"" + custom + "\"";
        for (std::size_t index = 1; index < params; ++index) challenge += ", x" + std::to_string(index) + "=\"v\"";
        return http_test::empty_reply(401, {{"WWW-Authenticate", challenge}});
    });
}

std::size_t requests_to(const World& world, std::string_view target) {
    std::size_t count = 0;
    for (const Request& one : world.resource.requests()) {
        if (one.target == target) ++count;
    }
    return count;
}

TEST(OAuthBoundsHttp, AChallengeAtItsParameterBoundIsUsedAndOneParameterMoreIsIgnored) {
    constexpr std::size_t bound = 4;
    for (const std::size_t params : {bound - 1, bound, bound + 1}) {
        SCOPED_TRACE(params);
        World world;
        challenge_with_params(world, params);
        neomcp::OAuthOptions options = world.options();
        options.bounds.max_challenge_params = bound;
        auto client = world.open(world.authorizer(std::move(options)));
        expect_signed_in_or(client, std::nullopt);
        // Within the bound the metadata the challenge names is read; beyond it the challenge is no challenge and the
        // well-known place is asked.
        EXPECT_EQ(requests_to(world, "/custom-prm"), params <= bound ? 1u : 0u);
        EXPECT_EQ(requests_to(world, "/.well-known/oauth-protected-resource/mcp"), params <= bound ? 0u : 1u);
    }
}

// ---- Authorization servers: max_authorization_servers

TEST(OAuthBoundsHttp, TheAuthorizationServerAtTheBoundIsUsedAndTheOneAfterItIsNot) {
    constexpr std::size_t bound = 3;
    for (const std::size_t position : {std::size_t{1}, bound, bound + 1}) {
        SCOPED_TRACE(position);
        World world;
        const std::string issuer = world.as.issuer();
        world.resource.edit_metadata([issuer, position](json& document) {
            json servers = json::array();
            for (std::size_t index = 1; index <= bound + 2; ++index) {
                servers.push_back(index == position ? issuer : "http://plain" + std::to_string(index) + ".example");
            }
            document["authorization_servers"] = servers;
        });
        neomcp::OAuthOptions options = world.options();
        options.bounds.max_authorization_servers = bound;
        auto client = world.open(world.authorizer(std::move(options)));
        expect_signed_in_or(client, position > bound ? std::optional<AuthorizationProblem>(AuthorizationProblem::refused_url)
                                                      : std::nullopt);
        // The list was longer than the bound, and the application is told so.
        bool told = false;
        for (const std::string& one : world.diagnostics_seen()) {
            if (one.find("more authorization servers") != std::string::npos) told = true;
        }
        EXPECT_TRUE(told);
    }
}

// ---- Scopes: max_scopes

TEST(OAuthBoundsHttp, TheScopesOfAChallengeAtTheBoundAreAskedAndTheRestIsLeftOut) {
    constexpr std::size_t bound = 3;
    for (const std::size_t count : {bound - 1, bound, bound + 1}) {
        SCOPED_TRACE(count);
        World world;
        std::vector<std::string> values;
        for (std::size_t index = 0; index < count; ++index) values.push_back("mcp:s" + std::to_string(index));
        std::string text;
        for (const std::string& one : values) text += (text.empty() ? "" : " ") + one;
        world.resource.challenge_scope(text);
        neomcp::OAuthOptions options = world.options();
        options.bounds.max_scopes = bound;
        auto client = world.open(world.authorizer(std::move(options)));
        expect_signed_in_or(client, std::nullopt);
        const auto prompts = world.person.prompts();
        ASSERT_EQ(prompts.size(), 1u);
        EXPECT_EQ(prompts.front().scopes.size(), std::min(count, bound));
        const auto asked = field_of(query_of(prompts.front().authorization_url), "scope");
        ASSERT_TRUE(asked);
        EXPECT_EQ(split_scopes(*asked), prompts.front().scopes);
        bool told = false;
        for (const std::string& one : world.diagnostics_seen()) {
            if (one.find("scopes") != std::string::npos) told = true;
        }
        EXPECT_EQ(told, count > bound);
    }
}

// ---- Time of one request to an authorization server: authorization_fetch_timeout

constexpr auto request_bound = std::chrono::milliseconds(600);

// The answer to a token or registration request that stops after the head and never goes on.
void stall_at(World& world, std::string target, int status, std::shared_ptr<http_test::Gate> never) {
    world.as.override_with([target = std::move(target), status, never](const Request& request) -> std::optional<http_test::Reply> {
        if (request.target != target) return std::nullopt;
        http_test::Reply reply;
        reply.send(http_test::head(status, {{"Content-Type", "application/json"}}, 4096)).send("{\"client_id\":");
        reply.wait(never);
        return reply;
    });
}

TEST(OAuthBoundsHttp, ATokenOrRegistrationRequestThatNeverEndsFailsAtTheRequestsBound) {
    for (const char* target : {"/register", "/token"}) {
        SCOPED_TRACE(target);
        World world;
        auto never = std::make_shared<http_test::Gate>();
        stall_at(world, target, std::string(target) == "/token" ? 200 : 201, never);
        neomcp::OAuthOptions options = world.options();
        options.bounds.authorization_fetch_timeout = request_bound;
        const auto started = BoundsClock::now();
        auto client = world.open(world.authorizer(std::move(options)));
        const auto took = BoundsClock::now() - started;
        expect_signed_in_or(client, AuthorizationProblem::timed_out);
        EXPECT_GE(took, request_bound);
        EXPECT_LT(took, std::chrono::seconds(10));
        never->open();
    }
}

// An answer that arrives well inside the bound is taken.
TEST(OAuthBoundsHttp, ATokenOrRegistrationAnswerInsideTheRequestsBoundIsTaken) {
    for (const char* target : {"/register", "/token"}) {
        SCOPED_TRACE(target);
        World world;
        AuthServer& as = world.as;
        const std::string path = target;
        as.override_with([&as, path](const Request& request) -> std::optional<http_test::Reply> {
            if (request.target != path) return std::nullopt;
            // The override runs on the server's thread without its lock: the wait delays the answer only.
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            return as.answer(request);
        });
        neomcp::OAuthOptions options = world.options();
        options.bounds.authorization_fetch_timeout = request_bound;
        auto client = world.open(world.authorizer(std::move(options)));
        expect_signed_in_or(client, std::nullopt);
    }
}

// ---- The person's time: prompt_timeout

TEST(OAuthBoundsHttp, APersonWhoAnswersInsideThePromptBoundIsTaken) {
    World world;
    const auto release = BoundsClock::now() + std::chrono::milliseconds(300);
    world.person.hold_until([release] { return BoundsClock::now() >= release; });
    neomcp::OAuthOptions options = world.options();
    options.bounds.prompt_timeout = std::chrono::milliseconds(2500);
    auto client = world.open(world.authorizer(std::move(options)));
    expect_signed_in_or(client, std::nullopt);
}

// ---- The error code: max_error_code_bytes

// ---- A refresh token: max_token_bytes

// ---- Holds of one request: HttpEndpoint::max_authorizations_per_request

// The resource refuses the first `refusals` requests whatever they carry, with a challenge that names its metadata.
void refuse_first(World& world, std::size_t refusals, std::shared_ptr<std::atomic<std::size_t>> seen) {
    const std::string metadata = world.resource.metadata_url();
    world.resource.override_with([metadata, refusals, seen](const Request& request) -> std::optional<http_test::Reply> {
        if (request.method != "POST" || request.target != "/mcp") return std::nullopt;
        if (seen->fetch_add(1) >= refusals) return std::nullopt;
        return http_test::empty_reply(401, {{"WWW-Authenticate", "Bearer resource_metadata=\"" + metadata + "\""}});
    });
}

TEST(OAuthBoundsHttp, ARequestIsHeldAsOftenAsItsBoundAndNoMore) {
    for (const std::size_t bound : {std::size_t{1}, std::size_t{2}, std::size_t{3}}) {
        for (const std::size_t refusals : {bound - 1, bound, bound + 1}) {
            if (refusals == 0) continue;  // the resource refuses a request without a token on its own
            SCOPED_TRACE("bound " + std::to_string(bound) + ", refused " + std::to_string(refusals) + " times");
            World world;
            auto seen = std::make_shared<std::atomic<std::size_t>>(0);
            refuse_first(world, refusals, seen);
            neomcp::OAuthOptions options = world.options();
            options.bounds.max_step_ups_per_operation = 20;  // the sign-ins in a row are not what is tried here
            auto client = open_with(world, world.authorizer(std::move(options)),
                                    [bound](neomcp::HttpEndpoint& described) { described.max_authorizations_per_request = bound; });
            if (refusals <= bound) {
                expect_signed_in_or(client, std::nullopt);
                EXPECT_EQ(world.person.prompts().size(), refusals);
            } else {
                expect_signed_in_or(client, AuthorizationProblem::attempts_exceeded);
                EXPECT_EQ(world.person.prompts().size(), bound);
                EXPECT_EQ(mcp_posts(world), bound + 1);
            }
        }
    }
}

// ---- Sign-ins of one operation in a row: max_step_ups_per_operation

// Every request with a good token is refused for want of a scope never asked for: one operation keeps signing in until
// the bound stops it, after max_step_ups_per_operation + 1 sign-ins.
TEST(OAuthBoundsHttp, AnOperationSignsInOneTimeMoreThanItsStepUpBound) {
    for (const std::size_t bound : {std::size_t{0}, std::size_t{1}, std::size_t{3}}) {
        SCOPED_TRACE(bound);
        World world;
        world.resource.escalate_scopes(true);
        neomcp::OAuthOptions options = world.options();
        options.bounds.max_step_ups_per_operation = bound;
        auto client = open_with(world, world.authorizer(std::move(options)),
                                [](neomcp::HttpEndpoint& described) { described.max_authorizations_per_request = 20; });
        expect_signed_in_or(client, AuthorizationProblem::attempts_exceeded);
        EXPECT_EQ(world.person.prompts().size(), bound + 1);
    }
}

// ---- Options that cannot work

TEST(OAuthBoundsHttp, AnyBoundOfZeroIsRefusedExceptTheStepUpsOfAnOperation) {
    World world;
    using Field = std::function<void(neomcp::OAuthBounds&)>;
    const std::vector<std::pair<const char*, Field>> fields = {
        {"max_metadata_bytes", [](neomcp::OAuthBounds& b) { b.max_metadata_bytes = 0; }},
        {"max_token_response_bytes", [](neomcp::OAuthBounds& b) { b.max_token_response_bytes = 0; }},
        {"max_token_bytes", [](neomcp::OAuthBounds& b) { b.max_token_bytes = 0; }},
        {"max_callback_bytes", [](neomcp::OAuthBounds& b) { b.max_callback_bytes = 0; }},
        {"max_challenge_params", [](neomcp::OAuthBounds& b) { b.max_challenge_params = 0; }},
        {"max_authorization_servers", [](neomcp::OAuthBounds& b) { b.max_authorization_servers = 0; }},
        {"max_scopes", [](neomcp::OAuthBounds& b) { b.max_scopes = 0; }},
        {"max_scope_bytes", [](neomcp::OAuthBounds& b) { b.max_scope_bytes = 0; }},
        {"max_error_code_bytes", [](neomcp::OAuthBounds& b) { b.max_error_code_bytes = 0; }},
        {"max_document_depth", [](neomcp::OAuthBounds& b) { b.max_document_depth = 0; }},
        {"authorization_fetch_timeout", [](neomcp::OAuthBounds& b) { b.authorization_fetch_timeout = std::chrono::milliseconds(0); }},
        {"prompt_timeout", [](neomcp::OAuthBounds& b) { b.prompt_timeout = std::chrono::milliseconds(0); }},
        {"authorization_timeout", [](neomcp::OAuthBounds& b) { b.authorization_timeout = std::chrono::milliseconds(0); }},
        {"max_token_lifetime", [](neomcp::OAuthBounds& b) { b.max_token_lifetime = std::chrono::seconds(0); }},
    };
    for (const auto& [name, zero] : fields) {
        SCOPED_TRACE(name);
        neomcp::OAuthOptions options = world.options();
        zero(options.bounds);
        const auto made = neomcp::OAuthAuthorizer::create(std::move(options));
        ASSERT_FALSE(made);
        EXPECT_EQ(made.error().kind, ClientErrorKind::invalid_options);
        EXPECT_NE(made.error().message.find("OAuthOptions::bounds"), std::string::npos) << made.error().message;
    }
    neomcp::OAuthOptions no_step_ups = world.options();
    no_step_ups.bounds.max_step_ups_per_operation = 0;
    EXPECT_TRUE(neomcp::OAuthAuthorizer::create(std::move(no_step_ups)));
}

}  // namespace
