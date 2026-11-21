// OAuthAuthorizer's prompt and bounds against a scripted authorization server and resource on loopback: the time
// bound of a whole sign-in, closing while the person signs in, plain and coroutine prompt handlers, tokens kept in a
// store across two authorizers, forget(), the options create() refuses, and no secret in any error or diagnostic.
#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include "oauth_rig.h"

namespace {

using namespace oauth_rig;
using neomcp::AuthorizationProblem;
using neomcp::ClientErrorKind;

constexpr auto short_bound = std::chrono::milliseconds(400);

// ---- The time bound of a sign-in

// The person never comes back. The whole sign-in ends at its bound, before the prompt's own, and every call it held
// fails with timed_out; the prompt handler is told to stop.
TEST(OAuthAuthorizerBounds, ASignInThatOutlastsItsBoundFailsTheHeldCalls) {
    World world;
    neomcp::OAuthOptions options = world.options();
    options.bounds.authorization_timeout = short_bound;
    options.bounds.prompt_timeout = std::chrono::minutes(10);
    auto authorizer = world.authorizer(std::move(options));
    auto client = world.open(authorizer);
    ASSERT_TRUE(client) << client.error().message;

    world.ledger.expire_all();
    world.person.set_mode(Person::Mode::wait_until_stopped);
    const auto started = std::chrono::steady_clock::now();
    auto first = std::async(std::launch::async, [&] { return client->request("tools/list"); });
    auto second = std::async(std::launch::async, [&] { return client->request("tools/list"); });
    const auto one = first.get();
    const auto two = second.get();
    const auto took = std::chrono::steady_clock::now() - started;
    for (const auto* result : {&one, &two}) {
        ASSERT_FALSE(*result);
        EXPECT_EQ(result->error().kind, ClientErrorKind::authorization_failed);
        ASSERT_TRUE(result->error().authorization);
        EXPECT_EQ(result->error().authorization->problem, AuthorizationProblem::timed_out);
    }
    EXPECT_LT(took, http_rig::wait_bound / 2);
    EXPECT_EQ(world.person.wait_stop_seen(), std::optional<bool>(true));
    EXPECT_EQ(world.person.prompts().size(), 2u);
    EXPECT_TRUE(client->close());
}

// The prompt's own bound, shorter than the sign-in's: prompt_timed_out.
TEST(OAuthAuthorizerBounds, APromptThatOutlastsItsBoundFails) {
    World world;
    neomcp::OAuthOptions options = world.options();
    options.bounds.prompt_timeout = short_bound;
    auto authorizer = world.authorizer(std::move(options));
    world.person.set_mode(Person::Mode::wait_until_stopped);
    auto client = world.open(authorizer);
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().kind, ClientErrorKind::authorization_failed);
    ASSERT_TRUE(client.error().authorization);
    EXPECT_EQ(client.error().authorization->problem, AuthorizationProblem::prompt_timed_out);
    EXPECT_EQ(world.person.wait_stop_seen(), std::optional<bool>(true));
}

// ---- Closing during the prompt

TEST(OAuthAuthorizerBounds, ClosingDuringThePromptEndsTheCallsClosed) {
    World world;
    auto authorizer = world.authorizer();
    auto client = world.open(authorizer);
    ASSERT_TRUE(client) << client.error().message;

    world.ledger.expire_all();
    world.person.set_mode(Person::Mode::wait_until_stopped);
    auto listing = std::async(std::launch::async, [&] { return client->request("tools/list"); });
    ASSERT_TRUE(world.person.wait_prompts(2));
    EXPECT_TRUE(client->close());
    const auto listed = listing.get();
    ASSERT_FALSE(listed);
    EXPECT_EQ(listed.error().kind, ClientErrorKind::closed);
    EXPECT_EQ(world.person.wait_stop_seen(), std::optional<bool>(true));
    // No token request followed.
    EXPECT_EQ(world.as.requests_to("/token").size(), 1u);
}

// ---- Prompt handlers

TEST(OAuthAuthorizerBounds, ACoroutinePromptHandlerRunsOnTheClientsThread) {
    World world;
    std::mutex mutex;
    std::vector<std::thread::id> threads;
    neomcp::OAuthOptions options = world.options();
    AuthServer* server = &world.as;
    options.prompt.handler([&, server](neomcp::AuthorizationPromptInfo info)
                               -> asio::awaitable<neomcp::AuthorizationPromptAnswer> {
        {
            std::lock_guard lock(mutex);
            threads.push_back(std::this_thread::get_id());
        }
        // The person takes a moment; the client's thread is not blocked meanwhile.
        asio::steady_timer pause(co_await asio::this_coro::executor, std::chrono::milliseconds(20));
        co_await pause.async_wait(asio::use_awaitable);
        co_return neomcp::AuthorizationPromptAnswer{Person::callback_for(info, *server)};
    });
    auto authorizer = world.authorizer(std::move(options));
    auto client = world.open(authorizer);
    ASSERT_TRUE(client) << client.error().message;
    const auto listed = client->request("tools/list");
    ASSERT_TRUE(listed) << listed.error().message;
    std::lock_guard lock(mutex);
    ASSERT_EQ(threads.size(), 1u);
    EXPECT_NE(threads.front(), std::this_thread::get_id());
    EXPECT_TRUE(client->close());
}

// A plain handler runs on a thread of its own, neither the application's nor the client's: it may block.
TEST(OAuthAuthorizerBounds, APlainPromptHandlerRunsOnAThreadOfItsOwn) {
    World world;
    std::thread::id coroutine_thread;
    neomcp::OAuthOptions coroutine_options = world.options();
    AuthServer* server = &world.as;
    coroutine_options.prompt.handler([&, server](const neomcp::AuthorizationPromptInfo& info)
                                         -> asio::awaitable<neomcp::AuthorizationPromptAnswer> {
        coroutine_thread = std::this_thread::get_id();
        co_return neomcp::AuthorizationPromptAnswer{Person::callback_for(info, *server)};
    });
    {
        auto client = world.open(world.authorizer(std::move(coroutine_options)));
        ASSERT_TRUE(client) << client.error().message;
        EXPECT_TRUE(client->close());
    }
    World other;
    auto client = other.open(other.authorizer());
    ASSERT_TRUE(client) << client.error().message;
    ASSERT_EQ(other.person.threads().size(), 1u);
    EXPECT_NE(other.person.threads().front(), std::this_thread::get_id());
    EXPECT_NE(other.person.threads().front(), coroutine_thread);
    EXPECT_TRUE(client->close());
}

TEST(OAuthAuthorizerBounds, APromptHandlerThatThrowsGivesUp) {
    World world;
    neomcp::OAuthOptions options = world.options();
    options.prompt.handler([](const neomcp::AuthorizationPromptInfo&) -> neomcp::AuthorizationPromptAnswer {
        throw std::runtime_error(std::string(planted_secret));
    });
    auto client = world.open(world.authorizer(std::move(options)));
    ASSERT_FALSE(client);
    ASSERT_TRUE(client.error().authorization);
    EXPECT_EQ(client.error().authorization->problem, AuthorizationProblem::prompt_cancelled);
    expect_clean(client.error(), {std::string(planted_secret)});
    for (const std::string& seen : world.diagnostics_seen()) expect_none_of(seen, {std::string(planted_secret)}, seen);
}

// ---- Tokens kept in a store

// A second authorizer with the store of the first: its first request is refused without a token, and the stored
// tokens serve it; the person is not asked and the client not registered again.
TEST(OAuthAuthorizerBounds, TokensPersistAcrossTwoAuthorizers) {
    World world;
    {
        auto client = world.open(world.authorizer());
        ASSERT_TRUE(client) << client.error().message;
        EXPECT_TRUE(client->close());
    }
    ASSERT_EQ(world.person.prompts().size(), 1u);
    auto again = world.authorizer();
    auto client = world.open(again);
    ASSERT_TRUE(client) << client.error().message;
    const auto listed = client->request("tools/list");
    ASSERT_TRUE(listed) << listed.error().message;
    EXPECT_EQ(world.person.prompts().size(), 1u);
    EXPECT_EQ(world.as.requests_to("/register").size(), 1u);
    EXPECT_EQ(world.as.requests_to("/token").size(), 1u);
    EXPECT_EQ(world.resource.authorizations().back(), "Bearer one-access-1");

    // forget() drops what this authorizer used: the next refusal needs the person again.
    again->forget();
    EXPECT_FALSE(world.store->tokens({world.resource.url(), world.as.issuer()}));
    world.ledger.expire_all();
    const auto relisted = client->request("tools/list");
    ASSERT_TRUE(relisted) << relisted.error().message;
    EXPECT_EQ(world.person.prompts().size(), 2u);
    EXPECT_TRUE(client->close());
}

// ---- Options

TEST(OAuthAuthorizerBounds, CreateRefusesOptionsThatCannotWork) {
    World world;
    const auto refused = [](neomcp::OAuthOptions options) {
        const auto made = neomcp::OAuthAuthorizer::create(std::move(options));
        return !made && made.error().kind == ClientErrorKind::invalid_options;
    };
    neomcp::OAuthOptions no_prompt = world.options();
    no_prompt.prompt = neomcp::AuthorizationPrompt{};
    EXPECT_TRUE(refused(no_prompt));
    for (const char* uri : {"http://example.com/callback", "https://app.example/cb#part", "not a url", ""}) {
        neomcp::OAuthOptions bad = world.options();
        bad.redirect_uri = uri;
        EXPECT_TRUE(refused(bad)) << uri;
    }
    for (const char* url : {"http://app.example/client.json", "https://app.example/", "https://app.example/a/../b"}) {
        neomcp::OAuthOptions bad = world.options();
        bad.client_id_metadata_url = std::string(url);
        EXPECT_TRUE(refused(bad)) << url;
    }
    neomcp::OAuthOptions zero = world.options();
    zero.bounds.authorization_timeout = std::chrono::milliseconds(0);
    EXPECT_TRUE(refused(zero));
    neomcp::OAuthOptions no_codes = world.options();
    no_codes.bounds.max_error_code_bytes = 0;
    EXPECT_TRUE(refused(no_codes));
    neomcp::OAuthOptions fine = world.options();
    fine.redirect_uri = "https://app.example/callback";
    EXPECT_FALSE(refused(fine));
}

// A client ID metadata document URL with a query is accepted, and reported once the authorizer serves a transport.
TEST(OAuthAuthorizerBounds, AMetadataDocumentURLWithAQueryIsReported) {
    World world;
    neomcp::OAuthOptions options = world.options();
    options.client_id_metadata_url = "https://app.example/client.json?v=1";
    auto client = world.open(world.authorizer(std::move(options)));
    ASSERT_TRUE(client) << client.error().message;
    EXPECT_TRUE(client->close());
    bool reported = false;
    for (const std::string& seen : world.diagnostics_seen()) {
        if (seen.find("client ID metadata document URL has a query") != std::string::npos) reported = true;
    }
    EXPECT_TRUE(reported);
}

// ---- No secret in any error or diagnostic

TEST(OAuthAuthorizerBounds, NoSecretInAnyErrorOrDiagnostic) {
    World world;
    world.as.issue_refresh_tokens(true);
    world.as.register_with_secret(true);
    std::mutex mutex;
    std::vector<std::string> states;
    std::vector<std::string> codes;
    world.person.answer_with([&](const neomcp::AuthorizationPromptInfo& info) {
        const std::string callback = Person::callback_for(info, world.as);
        std::lock_guard lock(mutex);
        states.push_back(field_of(query_of(info.authorization_url), "state").value_or(std::string()));
        codes.push_back(field_of(query_of(callback), "code").value_or(std::string()));
        return callback;
    });
    auto authorizer = world.authorizer();
    auto client = world.open(authorizer);
    ASSERT_TRUE(client) << client.error().message;

    std::vector<neomcp::ClientError> errors;
    // The token request is refused with a description; the refresh is refused first.
    world.ledger.expire_all();
    world.as.refuse_next_token("invalid_grant");
    world.person.answer_with([&](const neomcp::AuthorizationPromptInfo& info) {
        const std::string callback = Person::callback_for(info, world.as);
        std::lock_guard lock(mutex);
        states.push_back(field_of(query_of(info.authorization_url), "state").value_or(std::string()));
        codes.push_back(field_of(query_of(callback), "code").value_or(std::string()));
        world.as.refuse_next_token("access_denied");
        return callback;
    });
    auto first = client->request("tools/list");
    ASSERT_FALSE(first);
    ASSERT_TRUE(first.error().authorization);
    EXPECT_EQ(first.error().authorization->problem, AuthorizationProblem::token_request_failed);
    EXPECT_EQ(first.error().authorization->error_code, "access_denied");
    errors.push_back(first.error());

    // A callback with another state, and secrets in its query.
    world.person.answer_with([&](const neomcp::AuthorizationPromptInfo& info) {
        std::lock_guard lock(mutex);
        states.push_back(field_of(query_of(info.authorization_url), "state").value_or(std::string()));
        return info.redirect_uri + "?code=" + std::string(planted_secret) + "&state=other&error_description=" +
               std::string(planted_description);
    });
    auto second = client->request("tools/list");
    ASSERT_FALSE(second);
    ASSERT_TRUE(second.error().authorization);
    EXPECT_EQ(second.error().authorization->problem, AuthorizationProblem::state_mismatch);
    errors.push_back(second.error());

    // A callback that is not the redirect URI.
    world.person.answer_with([&](const neomcp::AuthorizationPromptInfo&) {
        return "https://elsewhere.example/cb?code=" + std::string(planted_secret);
    });
    auto third = client->request("tools/list");
    ASSERT_FALSE(third);
    errors.push_back(third.error());
    EXPECT_TRUE(client->close());

    std::vector<std::string> secrets = {std::string(planted_secret), std::string(planted_description),
                                        "one-access", "one-refresh"};
    {
        std::lock_guard lock(mutex);
        secrets.insert(secrets.end(), states.begin(), states.end());
        secrets.insert(secrets.end(), codes.begin(), codes.end());
    }
    for (const Request& one : world.as.requests_to("/token")) {
        if (auto verifier = field_of(one.body, "code_verifier")) secrets.push_back(*verifier);
    }
    for (const neomcp::ClientError& error : errors) expect_clean(error, secrets);
    for (const std::string& seen : world.diagnostics_seen()) expect_none_of(seen, secrets, seen);
}

// ---- The README's example

// Stand-ins for what the README leaves to the application.
void open_in_browser(const std::string&) {}
std::optional<std::string> wait_for_redirect(const std::string& redirect, std::stop_token) {
    return redirect + "?error=access_denied";
}

TEST(OAuthAuthorizerBounds, TheReadmeExampleBuilds) {
    neomcp::OAuthOptions oauth;
    oauth.redirect_uri = "http://127.0.0.1:8765/callback";
    oauth.client_name = "My tool";
    oauth.prompt.handler([](const neomcp::AuthorizationPromptInfo& prompt,
                             neomcp::AuthorizationPromptContext& context) {
        open_in_browser(prompt.authorization_url);  // the application's own
        // Wait at the redirect URI until the browser comes back, or until the sign-in is no longer waited for.
        std::optional<std::string> back = wait_for_redirect(prompt.redirect_uri, context.stop_token());
        return neomcp::AuthorizationPromptAnswer{back};
    });
    auto authorizer = neomcp::OAuthAuthorizer::create(std::move(oauth));
    ASSERT_TRUE(authorizer) << authorizer.error().message;

    neomcp::HttpEndpoint endpoint;
    endpoint.url = "https://mcp.example.com/mcp";
    endpoint.authorizer = *authorizer;
    EXPECT_NE(endpoint.authorizer, nullptr);
}

}  // namespace
