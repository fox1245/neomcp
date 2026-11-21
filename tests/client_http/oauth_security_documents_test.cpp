// What the client refuses in the documents and answers of a sign-in, over loopback against the scripted authorization
// server and resource: metadata documents one byte under, at and one byte over their bound and nested to their depth
// and one level deeper; a metadata answer that drips or never ends; token answers of another type, without a token,
// with a token that cannot be sent, over their bounds, or with a lifetime beyond any use; registration answers that
// change the redirect URI, give an identifier that is not a string, or refuse; a registration or token request answered
// with a redirect; and a prompt handler that never returns. No error and no diagnostic carries a secret.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "oauth_rig.h"

namespace {

using namespace oauth_rig;
using neomcp::AuthorizationProblem;
using neomcp::ClientErrorKind;
using Clock = std::chrono::steady_clock;

// Pads `document` with one more member so that its text is exactly `size` bytes.
void pad_to(json& document, std::size_t size) {
    document["pad"] = "";
    const std::size_t base = document.dump().size();
    ASSERT_LE(base, size);
    document["pad"] = std::string(size - base, 'a');
}

// A JSON value nested `levels` deep (one: an empty array).
json nested(std::size_t levels) {
    json value = json::array();
    for (std::size_t i = 1; i < levels; ++i) value = json::array({value});
    return value;
}

// The client opened, or failed with this problem; in either case nothing it reports carries a secret.
void expect_outcome(World& world, neomcp::Result<neomcp::Client>& client, std::optional<AuthorizationProblem> problem) {
    if (!problem) {
        ASSERT_TRUE(client) << client.error().message;
        EXPECT_TRUE(client->close());
        expect_quiet(world, {}, world.secrets());
        return;
    }
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().kind, ClientErrorKind::authorization_failed) << client.error().message;
    ASSERT_TRUE(client.error().authorization);
    EXPECT_EQ(client.error().authorization->problem, *problem);
    expect_quiet(world, {client.error()}, world.secrets());
}

// ---- Metadata documents at their bounds

constexpr std::size_t document_bound = 2048;

TEST(OAuthSecurity, AResourceMetadataDocumentAtItsBoundIsReadAndOneByteMoreIsNot) {
    for (const std::size_t size : {document_bound - 1, document_bound, document_bound + 1}) {
        SCOPED_TRACE(size);
        World world;
        world.resource.edit_metadata([size](json& document) { pad_to(document, size); });
        neomcp::OAuthOptions options = world.options();
        options.bounds.max_metadata_bytes = document_bound;
        auto client = world.open(world.authorizer(std::move(options)));
        const bool over = size > document_bound;
        expect_outcome(world, client,
                       over ? std::optional<AuthorizationProblem>(AuthorizationProblem::document_too_large)
                            : std::nullopt);
        // Over its bound, nothing of the document was used: no authorization server was asked.
        if (over) {
            EXPECT_EQ(world.as.server().count(), 0u);
        }
    }
}

TEST(OAuthSecurity, AnAuthorizationServerDocumentAtItsBoundIsReadAndOneByteMoreIsNot) {
    for (const std::size_t size : {document_bound - 1, document_bound, document_bound + 1}) {
        SCOPED_TRACE(size);
        World world;
        world.as.edit_metadata([size](json& document) { pad_to(document, size); });
        neomcp::OAuthOptions options = world.options();
        options.bounds.max_metadata_bytes = document_bound;
        auto client = world.open(world.authorizer(std::move(options)));
        const bool over = size > document_bound;
        expect_outcome(world, client,
                       over ? std::optional<AuthorizationProblem>(AuthorizationProblem::document_too_large)
                            : std::nullopt);
        if (over) {
            EXPECT_TRUE(world.as.requests_to("/register").empty());
        }
    }
}

// The outer object counts as one level.
TEST(OAuthSecurity, MetadataNestedToItsDepthIsReadAndOneLevelDeeperIsNot) {
    constexpr std::size_t depth = 8;
    for (const bool resource_document : {true, false}) {
        for (const std::size_t levels : {depth, depth + 1}) {
            SCOPED_TRACE(std::string(resource_document ? "resource" : "server") + " " + std::to_string(levels));
            World world;
            const auto deepen = [levels](json& document) { document["deep"] = nested(levels - 1); };
            if (resource_document) {
                world.resource.edit_metadata(deepen);
            } else {
                world.as.edit_metadata(deepen);
            }
            neomcp::OAuthOptions options = world.options();
            options.bounds.max_document_depth = depth;
            auto client = world.open(world.authorizer(std::move(options)));
            expect_outcome(world, client,
                           levels > depth ? std::optional<AuthorizationProblem>(AuthorizationProblem::discovery_failed)
                                          : std::nullopt);
        }
    }
}

// ---- Metadata that drips or never ends

constexpr auto fetch_bound = std::chrono::milliseconds(500);

// The answer's head arrives at once, its body a byte at a time: the request's own bound ends it, not the sign-in's.
TEST(OAuthSecurity, AMetadataAnswerThatDripsEndsAtTheRequestsBound) {
    World world;
    const std::string body = world.as.metadata_document().dump();
    world.as.override_with([body](const Request& request) -> std::optional<http_test::Reply> {
        if (request.target != "/.well-known/oauth-authorization-server") return std::nullopt;
        http_test::Reply reply;
        reply.send(http_test::head(200, {{"Content-Type", "application/json"}}, body.size()));
        for (const char c : body) reply.pause(std::chrono::milliseconds(50)).send(std::string(1, c));
        return reply;
    });
    neomcp::OAuthOptions options = world.options();
    options.bounds.authorization_fetch_timeout = fetch_bound;
    const auto started = Clock::now();
    auto client = world.open(world.authorizer(std::move(options)));
    const auto took = Clock::now() - started;
    expect_outcome(world, client, AuthorizationProblem::timed_out);
    ASSERT_FALSE(client);
    ASSERT_TRUE(client.error().authorization);
    EXPECT_EQ(client.error().authorization->url, world.as.issuer() + "/.well-known/oauth-authorization-server");
    EXPECT_LT(took, std::chrono::seconds(10));
    EXPECT_GE(took, fetch_bound);
    EXPECT_EQ(world.person.prompts().size(), 0u);
}

TEST(OAuthSecurity, AMetadataAnswerThatNeverEndsEndsAtTheRequestsBound) {
    World world;
    auto never = std::make_shared<http_test::Gate>();
    world.resource.override_with([never](const Request& request) -> std::optional<http_test::Reply> {
        if (request.target != "/.well-known/oauth-protected-resource/mcp") return std::nullopt;
        http_test::Reply reply;
        reply.send(http_test::head(200, {{"Content-Type", "application/json"}}, 4096)).send("{\"resource\":");
        reply.wait(never);
        return reply;
    });
    neomcp::OAuthOptions options = world.options();
    options.bounds.authorization_fetch_timeout = fetch_bound;
    const auto started = Clock::now();
    auto client = world.open(world.authorizer(std::move(options)));
    EXPECT_LT(Clock::now() - started, std::chrono::seconds(10));
    expect_outcome(world, client, AuthorizationProblem::timed_out);
    EXPECT_EQ(world.as.server().count(), 0u);
}

// ---- Token answers

// Answers the code exchange with `body` (a token in it is granted at the resource first) and status 200.
void answer_tokens_with(World& world, std::function<json()> make) {
    world.as.override_with([&world, make](const Request& request) -> std::optional<http_test::Reply> {
        if (request.target != "/token") return std::nullopt;
        const json body = make();
        if (body.contains("access_token") && body["access_token"].is_string()) {
            world.ledger.grant(body["access_token"].get<std::string>(), {});
        }
        return http_test::json_reply(200, body);
    });
}

json token_answer(std::string token, json type = "Bearer") {
    json body{{"access_token", std::move(token)}, {"expires_in", 3600}};
    if (!type.is_null()) body["token_type"] = std::move(type);
    return body;
}

TEST(OAuthSecurity, ATokenOfAnotherTypeIsNotSent) {
    for (const json& type : {json("DPoP"), json("mac"), json(nullptr), json(1)}) {
        SCOPED_TRACE(type.dump());
        World world;
        answer_tokens_with(world, [type] { return token_answer("other-type-token", type); });
        auto client = world.open(world.authorizer());
        expect_outcome(world, client, AuthorizationProblem::unsupported_token_type);
        for (const std::string& sent : world.resource.authorizations()) {
            EXPECT_EQ(sent.find("other-type-token"), std::string::npos);
        }
    }
    // Bearer in any case is the type the client sends.
    for (const char* type : {"bearer", "BEARER"}) {
        World world;
        answer_tokens_with(world, [type] { return token_answer("case-token", type); });
        auto client = world.open(world.authorizer());
        expect_outcome(world, client, std::nullopt);
        EXPECT_EQ(world.resource.authorizations().back(), "Bearer case-token");
    }
}

// An answer without a token, or with one that is not a string or cannot ride in a header field as it is, gives no
// token: a line end in it never reaches the resource as a field of its own.
TEST(OAuthSecurity, AnAnswerWithoutAUsableTokenGivesNone) {
    const std::vector<json> answers = {
        json{{"token_type", "Bearer"}},
        json{{"access_token", 12345}, {"token_type", "Bearer"}},
        json{{"access_token", ""}, {"token_type", "Bearer"}},
        json{{"access_token", "two words"}, {"token_type", "Bearer"}},
        json{{"access_token", "line\r\nX-Injected: 1"}, {"token_type", "Bearer"}},
        json{{"access_token", "a=b"}, {"token_type", "Bearer"}},
    };
    for (const json& answer : answers) {
        SCOPED_TRACE(answer.dump());
        World world;
        answer_tokens_with(world, [answer] { return answer; });
        auto client = world.open(world.authorizer());
        expect_outcome(world, client, AuthorizationProblem::token_request_failed);
        for (const Request& one : world.resource.requests()) EXPECT_FALSE(one.field("X-Injected"));
    }
}

TEST(OAuthSecurity, ATokenAtItsBoundIsSentAndOneByteLongerIsNot) {
    constexpr std::size_t bound = 512;
    for (const std::size_t size : {bound, bound + 1}) {
        SCOPED_TRACE(size);
        World world;
        const std::string token(size, 't');
        answer_tokens_with(world, [token] { return token_answer(token); });
        neomcp::OAuthOptions options = world.options();
        options.bounds.max_token_bytes = bound;
        auto client = world.open(world.authorizer(std::move(options)));
        if (size == bound) {
            expect_outcome(world, client, std::nullopt);
            EXPECT_EQ(world.resource.authorizations().back(), "Bearer " + token);
        } else {
            expect_outcome(world, client, AuthorizationProblem::token_request_failed);
        }
    }
}

TEST(OAuthSecurity, ATokenAnswerAtItsBoundIsReadAndOneByteMoreIsNot) {
    constexpr std::size_t bound = 2048;
    for (const std::size_t size : {bound - 1, bound, bound + 1}) {
        SCOPED_TRACE(size);
        World world;
        answer_tokens_with(world, [size] {
            json body = token_answer("bounded-token");
            pad_to(body, size);
            return body;
        });
        neomcp::OAuthOptions options = world.options();
        options.bounds.max_token_response_bytes = bound;
        auto client = world.open(world.authorizer(std::move(options)));
        expect_outcome(world, client,
                       size > bound ? std::optional<AuthorizationProblem>(AuthorizationProblem::document_too_large)
                                    : std::nullopt);
    }
}

// A lifetime beyond any use is kept as the longest one allowed; one that is not a positive number gives none.
TEST(OAuthSecurity, AHugeLifetimeIsCappedAndANonsenseOneIgnored) {
    struct Variant {
        json expires_in;
        bool expires;
    };
    const std::vector<Variant> variants = {{json(1e300), true},
                                           {json(9223372036854775807LL), true},
                                           {json(-5), false},
                                           {json(0), false},
                                           {json("3600"), false}};
    for (const Variant& variant : variants) {
        SCOPED_TRACE(variant.expires_in.dump());
        World world;
        answer_tokens_with(world, [&variant] {
            json body = token_answer("lifetime-token");
            body["expires_in"] = variant.expires_in;
            return body;
        });
        const auto before = std::chrono::system_clock::now();
        auto client = world.open(world.authorizer());
        const auto after = std::chrono::system_clock::now();
        expect_outcome(world, client, std::nullopt);
        const auto stored = world.store->tokens({world.resource.url(), world.as.issuer()});
        ASSERT_TRUE(stored);
        if (!variant.expires) {
            EXPECT_FALSE(stored->expires_at);
            continue;
        }
        ASSERT_TRUE(stored->expires_at);
        const auto lifetime = neomcp::OAuthDefaults::max_token_lifetime;
        EXPECT_GE(*stored->expires_at, before + lifetime - std::chrono::seconds(1));
        EXPECT_LE(*stored->expires_at, after + lifetime + std::chrono::seconds(1));
    }
}

// ---- Registration answers

TEST(OAuthSecurity, ARegistrationAnswerWithoutAStringIdentifierIsRefused) {
    for (const json& id : {json(42), json(""), json(nullptr), json::array({"one-client"})}) {
        SCOPED_TRACE(id.dump());
        World world;
        world.as.override_with([id](const Request& request) -> std::optional<http_test::Reply> {
            if (request.target != "/register") return std::nullopt;
            json body{{"redirect_uris", json::array({std::string(redirect_uri)})}};
            if (!id.is_null()) body["client_id"] = id;
            return http_test::json_reply(201, body);
        });
        auto client = world.open(world.authorizer());
        expect_outcome(world, client, AuthorizationProblem::registration_rejected);
        EXPECT_EQ(world.person.prompts().size(), 0u);
    }
}

// The registration the server answered names other redirect URIs than the one the client asked for: the person is
// not sent to a sign-in that would come back elsewhere.
TEST(OAuthSecurity, ARegistrationThatChangesTheRedirectUriIsRefused) {
    World world;
    world.as.override_with([](const Request& request) -> std::optional<http_test::Reply> {
        if (request.target != "/register") return std::nullopt;
        return http_test::json_reply(
            201, json{{"client_id", "changed-client"}, {"redirect_uris", json::array({"https://elsewhere.example/cb"})}});
    });
    auto client = world.open(world.authorizer());
    expect_outcome(world, client, AuthorizationProblem::registration_rejected);
    EXPECT_EQ(world.person.prompts().size(), 0u);
}

TEST(OAuthSecurity, ARefusedRegistrationTellsItsCodeOnly) {
    World world;
    world.as.override_with([](const Request& request) -> std::optional<http_test::Reply> {
        if (request.target != "/register") return std::nullopt;
        return http_test::json_reply(400, json{{"error", "invalid_client_metadata"},
                                               {"error_description", std::string(planted_description)}});
    });
    auto client = world.open(world.authorizer());
    expect_outcome(world, client, AuthorizationProblem::registration_rejected);
    ASSERT_FALSE(client);
    ASSERT_TRUE(client.error().authorization);
    EXPECT_EQ(client.error().authorization->error_code, "invalid_client_metadata");
}

// ---- Requests that carry secrets follow no redirect

// Not even within the server's own origin, where a followed redirect would answer as the endpoint would.
TEST(OAuthSecurity, ARegistrationOrTokenRequestFollowsNoRedirect) {
    for (const char* path : {"/register", "/token"}) {
        for (const bool same_origin : {true, false}) {
            SCOPED_TRACE(std::string(path) + (same_origin ? " to the same origin" : " to another origin"));
            World world;
            world.as.register_with_secret(true);
            AuthServer& as = world.as;
            // Answers at the place moved to as the endpoint itself would.
            ScriptedHttpServer elsewhere([&as, path](const Request& request) {
                Request moved = request;
                moved.target = path;
                return as.answer(moved);
            });
            const std::string target = same_origin ? world.as.issuer() + "/moved" : elsewhere.url(path);
            world.as.override_with([&as, path, target](const Request& request) -> std::optional<http_test::Reply> {
                if (request.target == "/moved") {
                    Request moved = request;
                    moved.target = path;
                    return as.answer(moved);
                }
                if (request.target != path) return std::nullopt;
                return http_test::empty_reply(307, {{"Location", target}});
            });
            auto client = world.open(world.authorizer());
            expect_outcome(world, client,
                           std::string(path) == "/token" ? AuthorizationProblem::token_request_failed
                                                         : AuthorizationProblem::registration_rejected);
            EXPECT_EQ(elsewhere.count(), 0u) << "the request went on to the other origin";
            EXPECT_TRUE(world.as.requests_to("/moved").empty()) << "the request was sent again where it was moved";
        }
    }
}

// ---- A prompt handler that never returns

// The handler ignores the request to stop; the sign-in still ends at its bound, and what the handler hands back later
// is not used.
TEST(OAuthSecurity, APromptHandlerThatNeverReturnsEndsAtTheSignInsBound) {
    std::atomic<bool> released{false};
    World world;
    world.person.hold_until([&released] { return released.load(); });
    neomcp::OAuthOptions options = world.options();
    options.bounds.authorization_timeout = std::chrono::milliseconds(400);
    const auto started = Clock::now();
    auto client = world.open(world.authorizer(std::move(options)));
    EXPECT_LT(Clock::now() - started, std::chrono::seconds(10));
    expect_outcome(world, client, AuthorizationProblem::timed_out);
    released = true;
    ASSERT_TRUE(world.person.wait_returned(1));
    EXPECT_TRUE(world.as.requests_to("/token").empty());
}

// ---- Error codes and refresh tokens at their bounds

// A code of exactly max_error_code_bytes reaches the application; one byte more does not, and the sign-in ends with the
// problem it would have ended with.
TEST(OAuthSecurity, AnErrorCodeAtItsBoundReachesTheApplicationAndOneByteMoreDoesNot) {
    constexpr std::size_t bound = 40;
    struct Source {
        const char* name;
        AuthorizationProblem problem;
    };
    for (const Source& source : {Source{"/register", AuthorizationProblem::registration_rejected},
                                 Source{"/token", AuthorizationProblem::token_request_failed},
                                 Source{"callback", AuthorizationProblem::authorization_denied}}) {
        for (const std::size_t size : {bound - 1, bound, bound + 1}) {
            SCOPED_TRACE(std::string(source.name) + " " + std::to_string(size));
            World world;
            const std::string code(size, 'e');
            const std::string path = source.name;
            if (path != "callback") {
                world.as.override_with([path, code](const Request& request) -> std::optional<http_test::Reply> {
                    if (request.target != path) return std::nullopt;
                    return http_test::json_reply(400, json{{"error", code}});
                });
            } else {
                world.person.answer_with([code](const neomcp::AuthorizationPromptInfo& info) {
                    const std::string state = field_of(query_of(info.authorization_url), "state").value_or("");
                    const std::vector<neomcp::http::FormField> fields = {{"error", code}, {"state", state}};
                    return info.redirect_uri + "?" + neomcp::http::form_encode(fields);
                });
            }
            neomcp::OAuthOptions options = world.options();
            options.bounds.max_error_code_bytes = bound;
            auto client = world.open(world.authorizer(std::move(options)));
            expect_outcome(world, client, source.problem);
            ASSERT_FALSE(client);
            ASSERT_TRUE(client.error().authorization);
            if (size <= bound) {
                EXPECT_EQ(client.error().authorization->error_code, code);
            } else {
                EXPECT_FALSE(client.error().authorization->error_code);
            }
        }
    }
}

}  // namespace
