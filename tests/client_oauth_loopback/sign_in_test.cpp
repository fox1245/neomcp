// A whole sign-in with OAuthAuthorizer through LoopbackRedirect, against the scripted authorization server and resource
// of the authorizer's tests. The browser is a plain HTTP request to the redirect URI, made when the application's
// function that opens the browser is called, with what the authorization server would have put in the redirect. Each
// test runs with the plain prompt and with the coroutine prompt.
#include <gtest/gtest.h>

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../client_http/oauth_rig.h"
#include "loopback_support.h"

namespace {

using namespace oauth_rig;
using loopback_test::connects;
using loopback_test::get;
using neomcp::AuthorizationProblem;
using neomcp::ClientErrorKind;
using neomcp::LoopbackRedirect;
using neomcp::LoopbackRedirectOptions;

// What the person's browser brings back to the redirect URI.
enum class Return {
    nothing,      // the person never comes back
    code,         // the code the authorization server issued, and the state of the request
    denied,       // error=access_denied and the state
    wrong_state,  // a code, and a state that is not the request's
};

class LoopbackSignIn : public ::testing::TestWithParam<bool> {
protected:
    ~LoopbackSignIn() override {
        for (std::thread& one : browsers_) {
            if (one.joinable()) one.join();
        }
    }

    // A redirect whose browser comes back as `how`, and an authorizer that uses it.
    std::shared_ptr<neomcp::OAuthAuthorizer> authorizer(Return how, neomcp::OAuthBounds bounds = {}) {
        how_ = how;
        LoopbackRedirectOptions options;
        options.open_browser = [this](const std::string& url) { open(url); };
        auto made = LoopbackRedirect::create(std::move(options));
        EXPECT_TRUE(made) << (made ? "" : made.error().message);
        if (!made) return nullptr;
        redirect_ = *made;
        neomcp::OAuthOptions oauth = world.options();
        oauth.redirect_uri = redirect_->redirect_uri();
        oauth.prompt = GetParam() ? redirect_->coroutine_prompt() : redirect_->plain_prompt();
        oauth.bounds = bounds;
        // The scripted server registers the redirect URI of the other tests; this one registers the one it is given.
        world.as.override_with([](const Request& request) -> std::optional<http_test::Reply> {
            if (request.method != "POST" || request.target != "/register") return std::nullopt;
            const json asked = json::parse(request.body);
            return http_test::json_reply(201, json{{"client_id", "loopback-client"}, {"redirect_uris", asked["redirect_uris"]}});
        });
        return world.authorizer(std::move(oauth));
    }

    // True once nothing listens on the redirect's port; the port is closed by the prompt's own thread when the client
    // ends the prompt, so it is waited for.
    bool port_closes() const {
        const auto until = std::chrono::steady_clock::now() + loopback_test::patience;
        while (std::chrono::steady_clock::now() < until) {
            if (!connects(redirect_->port())) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    std::vector<std::string> opened() const {
        std::lock_guard lock(mutex_);
        return opened_;
    }

    World world;
    std::shared_ptr<LoopbackRedirect> redirect_;

private:
    void open(const std::string& url) {
        {
            std::lock_guard lock(mutex_);
            opened_.push_back(url);
        }
        if (how_ == Return::nothing) return;
        const std::string query = query_of(url);
        const std::string state = field_of(query, "state").value_or(std::string());
        std::vector<neomcp::http::FormField> fields;
        switch (how_) {
            case Return::code: {
                const auto scope = field_of(query, "scope");
                fields.push_back({"code", world.as.issue_code(scope ? split_scopes(*scope) : std::vector<std::string>{})});
                fields.push_back({"state", state});
                break;
            }
            case Return::denied:
                fields.push_back({"error", "access_denied"});
                fields.push_back({"state", state});
                break;
            case Return::wrong_state:
                fields.push_back({"code", std::string(planted_secret)});
                fields.push_back({"state", "other"});
                break;
            case Return::nothing: break;
        }
        const std::string target = "/callback?" + neomcp::http::form_encode(fields);
        const std::uint16_t port = redirect_->port();
        std::lock_guard lock(mutex_);
        browsers_.emplace_back([port, target] { loopback_test::get(port, target); });
    }

    mutable std::mutex mutex_;
    Return how_ = Return::code;
    std::vector<std::string> opened_;
    std::vector<std::thread> browsers_;
};

TEST_P(LoopbackSignIn, SignsInThroughTheRedirectAndClosesThePort) {
    auto client = world.open(authorizer(Return::code));
    ASSERT_TRUE(client) << client.error().message;
    const auto listed = client->request("tools/list");
    ASSERT_TRUE(listed) << listed.error().message;

    // The authorization URL the application was given names the redirect URI of the loopback.
    ASSERT_EQ(opened().size(), 1u);
    EXPECT_EQ(field_of(query_of(opened().front()), "redirect_uri"), redirect_->redirect_uri());
    EXPECT_EQ(world.resource.authorizations().back(), "Bearer one-access-1");
    EXPECT_EQ(world.as.requests_to("/token").size(), 1u);
    EXPECT_TRUE(port_closes());
    EXPECT_TRUE(redirect_->last_problem().empty());
    EXPECT_TRUE(client->close());
}

TEST_P(LoopbackSignIn, AnAuthorizationServerThatRefusedEndsTheSignInWithItsCode) {
    auto client = world.open(authorizer(Return::denied));
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().kind, ClientErrorKind::authorization_failed);
    ASSERT_TRUE(client.error().authorization);
    EXPECT_EQ(client.error().authorization->problem, AuthorizationProblem::authorization_denied);
    EXPECT_EQ(client.error().authorization->error_code, "access_denied");
    EXPECT_TRUE(world.as.requests_to("/token").empty());
    EXPECT_TRUE(port_closes());
}

// The authorizer makes every check of the callback; the redirect hands it over untouched, and nothing of it leaks.
TEST_P(LoopbackSignIn, ACallbackWithAnotherStateIsRejectedByTheAuthorizer) {
    auto client = world.open(authorizer(Return::wrong_state));
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().kind, ClientErrorKind::authorization_failed);
    ASSERT_TRUE(client.error().authorization);
    EXPECT_EQ(client.error().authorization->problem, AuthorizationProblem::state_mismatch);
    EXPECT_TRUE(world.as.requests_to("/token").empty());
    expect_clean(client.error(), world.secrets());
    EXPECT_TRUE(port_closes());
}

TEST_P(LoopbackSignIn, ABrowserThatNeverComesBackEndsAtThePromptsTimeAndClosesThePort) {
    neomcp::OAuthBounds bounds;
    bounds.prompt_timeout = std::chrono::milliseconds(400);
    auto client = world.open(authorizer(Return::nothing, bounds));
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().kind, ClientErrorKind::authorization_failed);
    ASSERT_TRUE(client.error().authorization);
    EXPECT_EQ(client.error().authorization->problem, AuthorizationProblem::prompt_timed_out);
    ASSERT_EQ(opened().size(), 1u);
    EXPECT_TRUE(port_closes());
    // Cancelling the wait is not a failure of the redirect.
    EXPECT_TRUE(redirect_->last_problem().empty());
}

INSTANTIATE_TEST_SUITE_P(LoopbackRedirect, LoopbackSignIn, ::testing::Bool(), [](const auto& info) {
    return info.param ? std::string("CoroutinePrompt") : std::string("PlainPrompt");
});

}  // namespace
