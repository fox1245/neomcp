// OAuthAuthorizer against a host of the test's own instead of a transport: every request of a sign-in reaches the
// host (the hosts are under .invalid, so a request made any other way cannot be answered), the time bound ends a
// sign-in whose request never completes, and detach() ends a sign-in during the prompt. Each test runs the client's
// thread itself.
#include <gtest/gtest.h>

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>

#include "client_http/authorization_gate.h"
#include "oauth_rig.h"

namespace {

using namespace oauth_rig;
using neomcp::AuthorizationFetch;
using neomcp::AuthorizationFetchPurpose;
using neomcp::AuthorizationFetchResult;

constexpr std::string_view endpoint = "https://mcp.invalid/mcp";
constexpr std::string_view issuer = "https://as.invalid";

AuthorizationFetchResult json_result(int status, const json& body) {
    AuthorizationFetchResult result;
    result.status = status;
    result.media_type = "application/json";
    result.headers.push_back({"Content-Type", "application/json"});
    result.body = body.dump();
    return result;
}

class FakeHost final : public neomcp::AuthorizationHost {
public:
    explicit FakeHost(asio::io_context& io) : io_(io) {}

    asio::any_io_executor executor() const override { return io_.get_executor(); }
    const std::string& endpoint_url() const override { return endpoint_; }
    asio::awaitable<AuthorizationFetchResult> fetch(AuthorizationFetch request) override {
        seen.push_back(request);
        if (stall && request.purpose == *stall) {
            stalled = std::make_shared<asio::steady_timer>(io_, std::chrono::hours(1));
            const std::shared_ptr<asio::steady_timer> held = stalled;
            std::error_code ignored;
            co_await held->async_wait(asio::redirect_error(asio::use_awaitable, ignored));
        }
        co_return answer(request);
    }
    void diagnostic(std::string message) override { diagnostics.push_back(std::move(message)); }

    std::vector<AuthorizationFetch> seen;
    std::vector<std::string> diagnostics;
    std::optional<AuthorizationFetchPurpose> stall;  // a request of this purpose completes only when released
    std::shared_ptr<asio::steady_timer> stalled;
    // Each token answer is a new access token with a refresh token, and a refresh is answered as a code is.
    bool rotate = false;

private:
    AuthorizationFetchResult answer(const AuthorizationFetch& request) {
        const std::string as(issuer);
        if (request.url == "https://mcp.invalid/.well-known/oauth-protected-resource/mcp") {
            return json_result(200, json{{"resource", endpoint_}, {"authorization_servers", json::array({as})}});
        }
        if (request.url == as + "/.well-known/oauth-authorization-server") {
            return json_result(200, json{{"issuer", as},
                                         {"authorization_endpoint", as + "/authorize"},
                                         {"token_endpoint", as + "/token"},
                                         {"registration_endpoint", as + "/register"},
                                         {"code_challenge_methods_supported", json::array({"S256"})}});
        }
        if (request.url == as + "/register") return json_result(201, json{{"client_id", "host-client"}});
        if (request.url == as + "/token") {
            if (!rotate) return json_result(200, json{{"access_token", "host-access"}, {"token_type", "Bearer"}});
            const std::string issued = std::to_string(++issued_);
            return json_result(200, json{{"access_token", "host-access-" + issued},
                                         {"token_type", "Bearer"},
                                         {"refresh_token", "host-refresh-" + issued}});
        }
        return json_result(404, json::object());
    }

    asio::io_context& io_;
    std::string endpoint_{endpoint};
    std::size_t issued_ = 0;
};

// The client's thread, run by the test, and the replies the authorizer gives.
struct Rig {
    asio::io_context io;
    FakeHost host{io};
    std::vector<std::optional<neomcp::CallFailure>> answers;
    std::shared_ptr<neomcp::detail::http_client::ReplyChannel> channel =
        std::make_shared<neomcp::detail::http_client::ReplyChannel>(
            io.get_executor(),
            [this](std::uint64_t, std::optional<neomcp::CallFailure> failure) { answers.push_back(std::move(failure)); });

    // The person: signs in with a code at once (once `hold` is cleared), or waits until stopped.
    bool wait_for_stop = false;
    std::atomic<bool> hold{false};
    std::atomic<bool> prompted{false};
    std::atomic<int> prompts{0};
    std::atomic<bool> stop_seen{false};

    neomcp::OAuthOptions options() {
        neomcp::OAuthOptions made;
        made.redirect_uri = std::string(redirect_uri);
        made.prompt.handler([this](const neomcp::AuthorizationPromptInfo& info,
                                      neomcp::AuthorizationPromptContext& context) -> neomcp::AuthorizationPromptAnswer {
            prompted = true;
            ++prompts;
            const auto until = std::chrono::steady_clock::now() + http_rig::wait_bound;
            while (hold && !context.cancelled() && std::chrono::steady_clock::now() < until) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (wait_for_stop) {
                while (!context.cancelled() && std::chrono::steady_clock::now() < until) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                stop_seen = context.cancelled();
                return {};
            }
            const auto state = field_of(query_of(info.authorization_url), "state");
            std::vector<neomcp::http::FormField> fields;
            fields.push_back({"code", "host-code"});
            fields.push_back({"state", state.value_or(std::string())});
            return {info.redirect_uri + "?" + neomcp::http::form_encode(fields)};
        });
        return made;
    }

    void refuse(neomcp::OAuthAuthorizer& authorizer) {
        refuse(authorizer, {"POST", std::string(endpoint), 7, nullptr}, 0, 1);
    }

    // A 401 to `request`, which carried credentials of `generation`; the gate's flow `flow` waits for the answer.
    void refuse(neomcp::OAuthAuthorizer& authorizer, neomcp::HttpRequestInfo request, std::uint64_t generation,
                std::uint64_t flow) {
        asio::post(io, [this, &authorizer, request = std::move(request), generation, flow] {
            authorizer.challenge(request, 401, {}, generation,
                                 neomcp::detail::http_client::ReplyChannel::reply(channel, flow));
        });
    }

    // What the host was asked and told, for a failure message.
    std::string seen_text() const {
        std::string text;
        for (const AuthorizationFetch& one : host.seen) text += "\n  fetched " + one.url;
        for (const std::string& one : host.diagnostics) text += "\n  told " + one;
        return text;
    }

    // Runs the client's thread until `done` holds or the bound lapses.
    template <class F>
    bool run_until(F done, std::chrono::milliseconds bound = http_rig::wait_bound) {
        // The thread stopped when its work ran out at the end of the last run.
        if (io.stopped()) io.restart();
        auto guard = asio::make_work_guard(io);
        const auto until = std::chrono::steady_clock::now() + bound;
        while (!done() && std::chrono::steady_clock::now() < until) io.run_one_for(std::chrono::milliseconds(10));
        return done();
    }
};

TEST(OAuthAuthorizerHost, EveryRequestOfASignInGoesThroughTheHost) {
    Rig rig;
    auto made = neomcp::OAuthAuthorizer::create(rig.options());
    ASSERT_TRUE(made) << made.error().message;
    neomcp::OAuthAuthorizer& authorizer = **made;
    authorizer.attach(rig.host);
    rig.refuse(authorizer);
    ASSERT_TRUE(rig.run_until([&] { return !rig.answers.empty(); }));
    EXPECT_EQ(rig.answers.front(), std::nullopt);  // retry

    std::vector<AuthorizationFetchPurpose> purposes;
    for (const AuthorizationFetch& one : rig.host.seen) purposes.push_back(one.purpose);
    EXPECT_EQ(purposes, (std::vector<AuthorizationFetchPurpose>{AuthorizationFetchPurpose::resource_metadata,
                                                                 AuthorizationFetchPurpose::server_metadata,
                                                                 AuthorizationFetchPurpose::registration,
                                                                 AuthorizationFetchPurpose::token}));
    for (const AuthorizationFetch& one : rig.host.seen) {
        for (const neomcp::HttpHeader& field : one.headers) EXPECT_NE(field.name, "Authorization") << one.url;
        EXPECT_GT(one.max_body_bytes, 0u);
        EXPECT_GT(one.timeout.count(), 0);
    }
    EXPECT_EQ(rig.host.seen.back().method, neomcp::AuthorizationFetchMethod::post);
    const neomcp::Credential credential = authorizer.authorization({"POST", std::string(endpoint), 7, nullptr});
    EXPECT_EQ(credential.value, "Bearer host-access");
    EXPECT_EQ(credential.generation, 1u);
    authorizer.detach();
}

// A request that never completes: the sign-in ends at its bound with timed_out, and nothing reaches the host after.
TEST(OAuthAuthorizerHost, TheTimeBoundEndsASignInWhoseRequestNeverCompletes) {
    Rig rig;
    rig.host.stall = AuthorizationFetchPurpose::token;
    neomcp::OAuthOptions options = rig.options();
    options.bounds.authorization_timeout = std::chrono::milliseconds(200);
    auto made = neomcp::OAuthAuthorizer::create(std::move(options));
    ASSERT_TRUE(made) << made.error().message;
    neomcp::OAuthAuthorizer& authorizer = **made;
    authorizer.attach(rig.host);
    rig.refuse(authorizer);
    ASSERT_TRUE(rig.run_until([&] { return !rig.answers.empty(); }));
    ASSERT_TRUE(rig.answers.front());
    EXPECT_EQ(rig.answers.front()->kind, neomcp::CallFailureKind::authorization_failed);
    ASSERT_TRUE(rig.answers.front()->authorization);
    EXPECT_EQ(rig.answers.front()->authorization->problem, neomcp::AuthorizationProblem::timed_out);
    const std::size_t seen = rig.host.seen.size();
    rig.run_until([] { return false; }, std::chrono::milliseconds(50));
    EXPECT_EQ(rig.host.seen.size(), seen);
    EXPECT_EQ(authorizer.authorization({"POST", std::string(endpoint), 7, nullptr}).value, "");
    // The request completes after all: its answer belongs to no sign-in.
    ASSERT_TRUE(rig.host.stalled);
    rig.host.stalled->cancel();
    rig.run_until([] { return false; }, std::chrono::milliseconds(50));
    EXPECT_EQ(rig.answers.size(), 1u);
    EXPECT_EQ(authorizer.authorization({"POST", std::string(endpoint), 7, nullptr}).value, "");
    authorizer.detach();
}

// detach() during the prompt: the prompt handler is told to stop, the reply is answered, and what the handler returns
// afterwards changes nothing.
TEST(OAuthAuthorizerHost, DetachDuringThePromptEndsTheSignIn) {
    Rig rig;
    rig.wait_for_stop = true;
    auto made = neomcp::OAuthAuthorizer::create(rig.options());
    ASSERT_TRUE(made) << made.error().message;
    neomcp::OAuthAuthorizer& authorizer = **made;
    authorizer.attach(rig.host);
    rig.refuse(authorizer);
    ASSERT_TRUE(rig.run_until([&] { return rig.prompted.load(); }));
    asio::post(rig.io, [&] { authorizer.detach(); });
    ASSERT_TRUE(rig.run_until([&] { return !rig.answers.empty() && rig.stop_seen.load(); }));
    ASSERT_TRUE(rig.answers.front());
    const std::size_t seen = rig.host.seen.size();
    rig.run_until([] { return false; }, std::chrono::milliseconds(50));
    EXPECT_EQ(rig.host.seen.size(), seen);
    EXPECT_EQ(rig.answers.size(), 1u);
}

// A refusal that reaches the authorizer while a sign-in runs (the transport waits for one answer per generation, so
// this is a refusal it could not join itself) waits for that sign-in: one prompt, one token request, two retries.
TEST(OAuthAuthorizerHost, ARefusalDuringASignInJoinsIt) {
    Rig rig;
    rig.hold = true;
    auto made = neomcp::OAuthAuthorizer::create(rig.options());
    ASSERT_TRUE(made) << made.error().message;
    neomcp::OAuthAuthorizer& authorizer = **made;
    authorizer.attach(rig.host);
    rig.refuse(authorizer, {"POST", std::string(endpoint), 7, nullptr}, 0, 1);
    ASSERT_TRUE(rig.run_until([&] { return rig.prompts.load() == 1; }));
    rig.refuse(authorizer, {"POST", std::string(endpoint), 8, nullptr}, 0, 2);
    rig.run_until([] { return false; }, std::chrono::milliseconds(50));
    EXPECT_TRUE(rig.answers.empty());
    rig.hold = false;
    ASSERT_TRUE(rig.run_until([&] { return rig.answers.size() == 2; }));
    EXPECT_EQ(rig.answers[0], std::nullopt);
    EXPECT_EQ(rig.answers[1], std::nullopt);
    EXPECT_EQ(rig.prompts.load(), 1);
    std::size_t tokens = 0;
    for (const AuthorizationFetch& one : rig.host.seen) {
        if (one.purpose == AuthorizationFetchPurpose::token) ++tokens;
    }
    EXPECT_EQ(tokens, 1u);
    authorizer.detach();
}

// The stream of the connection is refused anew whenever its token expires. Each refusal starts a sign-in (here a
// refresh); since the stream is accepted between them, the bound on sign-ins in a row never ends it.
TEST(OAuthAuthorizerHost, AnAcceptedRequestStartsItsCountOfSignInsAgain) {
    Rig rig;
    rig.host.rotate = true;
    auto made = neomcp::OAuthAuthorizer::create(rig.options());
    ASSERT_TRUE(made) << made.error().message;
    neomcp::OAuthAuthorizer& authorizer = **made;
    authorizer.attach(rig.host);
    const neomcp::HttpRequestInfo stream{"GET", std::string(endpoint), neomcp::connection_unit, nullptr};
    const std::size_t cycles = neomcp::OAuthDefaults::max_step_ups_per_operation + 3;
    for (std::size_t cycle = 0; cycle < cycles; ++cycle) {
        const neomcp::Credential carried = authorizer.authorization(stream);
        rig.refuse(authorizer, stream, carried.generation, cycle + 1);
        ASSERT_TRUE(rig.run_until([&] { return rig.answers.size() == cycle + 1; })) << cycle << rig.seen_text();
        ASSERT_EQ(rig.answers.back(), std::nullopt) << "cycle " << cycle;
        EXPECT_EQ(authorizer.authorization(stream).value, "Bearer host-access-" + std::to_string(cycle + 1));
        authorizer.accepted(stream);
    }
    EXPECT_EQ(rig.prompts.load(), 1);
    authorizer.detach();
}

}  // namespace
