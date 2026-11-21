// A server elsewhere cannot point the client at this machine: with an endpoint on another host, an authorization
// server, a metadata document or an endpoint of authorization named on a loopback address, a name of this machine or
// an address that reaches it is refused before any request goes there. A remote endpoint must be https, which the
// loopback peers of the other tests do not serve, so these run the authorizer against a host of the test's own that
// answers each request from a table and records it; the client's thread is run by the test. Last, the gate of the
// transport, which gives a token only to a request of the endpoint's origin, on its own.
#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include "auth_rig.h"
#include "client_http/authorization_gate.h"
#include "client_http/url_guard.h"
#include "oauth_rig.h"

namespace {

using namespace oauth_rig;
using neomcp::AuthorizationFetch;
using neomcp::AuthorizationFetchResult;

constexpr std::string_view remote_endpoint = "https://mcp.example.test/mcp";
constexpr std::string_view remote_metadata = "https://mcp.example.test/.well-known/oauth-protected-resource/mcp";
constexpr std::string_view remote_issuer = "https://as.example.test";

// Addresses and names that reach this machine, in the forms a URL may write them.
const std::vector<std::string> this_machine = {
    "http://127.0.0.1:8080", "https://127.0.0.1",    "http://127.9.9.9",     "https://localhost",
    "https://LOCALHOST",     "https://localhost.",   "https://app.localhost", "http://[::1]:8080",
    "https://[::1]",         "https://[::ffff:127.0.0.1]", "https://0.0.0.0", "https://[::]",
    "https://127.1",         "https://2130706433",   "https://0x7f000001",   "https://0177.0.0.1",
};

AuthorizationFetchResult json_result(int status, const json& body) {
    AuthorizationFetchResult result;
    result.status = status;
    result.media_type = "application/json";
    result.headers.push_back({"Content-Type", "application/json"});
    result.body = body.dump();
    return result;
}

json server_document() {
    const std::string as(remote_issuer);
    return json{{"issuer", as},
                {"authorization_endpoint", as + "/authorize"},
                {"token_endpoint", as + "/token"},
                {"registration_endpoint", as + "/register"},
                {"code_challenge_methods_supported", json::array({"S256"})}};
}

class TableHost final : public neomcp::AuthorizationHost {
public:
    explicit TableHost(asio::io_context& io) : io_(io) {
        const std::string as(remote_issuer);
        answers[std::string(remote_metadata)] = json_result(
            200, json{{"resource", std::string(remote_endpoint)}, {"authorization_servers", json::array({as})}});
        answers[as + "/.well-known/oauth-authorization-server"] = json_result(200, server_document());
        answers[as + "/register"] = json_result(201, json{{"client_id", "table-client"}});
        answers[as + "/token"] = json_result(200, json{{"access_token", "table-access"}, {"token_type", "Bearer"}});
    }

    asio::any_io_executor executor() const override { return io_.get_executor(); }
    const std::string& endpoint_url() const override { return endpoint_; }
    asio::awaitable<AuthorizationFetchResult> fetch(AuthorizationFetch request) override {
        fetched.push_back(request.url);
        const auto found = answers.find(request.url);
        co_return found == answers.end() ? json_result(404, json::object()) : found->second;
    }
    void diagnostic(std::string message) override { diagnostics.push_back(std::move(message)); }

    std::map<std::string, AuthorizationFetchResult> answers;
    std::vector<std::string> fetched;
    std::vector<std::string> diagnostics;

private:
    asio::io_context& io_;
    std::string endpoint_{remote_endpoint};
};

struct HostRig {
    asio::io_context io;
    TableHost host{io};
    std::vector<std::optional<neomcp::CallFailure>> answers;
    std::shared_ptr<neomcp::detail::http_client::ReplyChannel> channel =
        std::make_shared<neomcp::detail::http_client::ReplyChannel>(
            io.get_executor(),
            [this](std::uint64_t, std::optional<neomcp::CallFailure> failure) { answers.push_back(std::move(failure)); });

    static neomcp::OAuthOptions options() {
        neomcp::OAuthOptions made;
        made.redirect_uri = std::string(redirect_uri);
        made.prompt.handler([](const neomcp::AuthorizationPromptInfo& info) -> neomcp::AuthorizationPromptAnswer {
            std::vector<neomcp::http::FormField> fields = {
                {"code", "table-code"}, {"state", field_of(query_of(info.authorization_url), "state").value_or("")}};
            return {info.redirect_uri + "?" + neomcp::http::form_encode(fields)};
        });
        return made;
    }

    // One refusal with these challenges, run until the authorizer answers it; the answer, or nothing.
    std::optional<std::optional<neomcp::CallFailure>> refuse(const std::vector<std::string>& challenges = {}) {
        auto made = neomcp::OAuthAuthorizer::create(options());
        EXPECT_TRUE(made);
        if (!made) return std::nullopt;
        std::shared_ptr<neomcp::OAuthAuthorizer> authorizer = *made;
        authorizer->attach(host);
        asio::post(io, [&] {
            authorizer->challenge({"POST", std::string(remote_endpoint), 7, nullptr}, 401, challenges, 0,
                                  neomcp::detail::http_client::ReplyChannel::reply(channel, 1));
        });
        auto guard = asio::make_work_guard(io);
        const auto until = std::chrono::steady_clock::now() + http_rig::wait_bound;
        while (answers.empty() && std::chrono::steady_clock::now() < until) {
            io.run_one_for(std::chrono::milliseconds(10));
        }
        authorizer->detach();
        if (answers.empty()) return std::nullopt;
        return answers.front();
    }
};

// The answer was a refusal of this URL's kind, and no request went to this machine.
void expect_refused_here(const std::optional<std::optional<neomcp::CallFailure>>& answer, const TableHost& host,
                         std::string_view named) {
    ASSERT_TRUE(answer) << "no answer";
    ASSERT_TRUE(*answer) << named << " was admitted";
    ASSERT_TRUE((*answer)->authorization);
    EXPECT_EQ((*answer)->authorization->problem, neomcp::AuthorizationProblem::refused_url);
    for (const std::string& url : host.fetched) {
        const auto parts = neomcp::detail::oauth::read_url(url);
        ASSERT_TRUE(parts) << url;
        EXPECT_EQ(url.find(std::string(named)), std::string::npos) << "a request went to " << url;
    }
}

// The control: the host's table signs in.
TEST(OAuthSecurity, AnEndpointElsewhereSignsInThroughItsHost) {
    HostRig rig;
    const auto answer = rig.refuse();
    ASSERT_TRUE(answer);
    EXPECT_FALSE(*answer) << (*answer)->message;
    EXPECT_EQ(rig.host.fetched.size(), 4u);
}

TEST(OAuthSecurity, AnEndpointElsewhereCannotNameThisMachineAsItsAuthorizationServer) {
    for (const std::string& named : this_machine) {
        SCOPED_TRACE(named);
        HostRig rig;
        rig.host.answers[std::string(remote_metadata)] = json_result(
            200, json{{"resource", std::string(remote_endpoint)}, {"authorization_servers", json::array({named})}});
        const auto answer = rig.refuse();
        expect_refused_here(answer, rig.host, named);
        EXPECT_EQ(rig.host.fetched, (std::vector<std::string>{std::string(remote_metadata)}));
    }
}

// A list that names this machine first and a server elsewhere after: the one elsewhere is used, and the skipped one
// is reported without its address.
TEST(OAuthSecurity, AnAuthorizationServerOnThisMachineIsSkippedForTheNextOne) {
    HostRig rig;
    rig.host.answers[std::string(remote_metadata)] =
        json_result(200, json{{"resource", std::string(remote_endpoint)},
                              {"authorization_servers", json::array({"http://127.0.0.1:8080", remote_issuer})}});
    const auto answer = rig.refuse();
    ASSERT_TRUE(answer);
    EXPECT_FALSE(*answer);
    for (const std::string& url : rig.host.fetched) EXPECT_EQ(url.find("127.0.0.1"), std::string::npos) << url;
    bool reported = false;
    for (const std::string& seen : rig.host.diagnostics) {
        if (seen.find("not admitted") != std::string::npos) reported = true;
        EXPECT_EQ(seen.find("127.0.0.1"), std::string::npos) << seen;
    }
    EXPECT_TRUE(reported);
}

TEST(OAuthSecurity, AnAuthorizationServerElsewhereCannotNameEndpointsOnThisMachine) {
    for (const char* member : {"token_endpoint", "authorization_endpoint", "registration_endpoint"}) {
        for (const std::string& named : this_machine) {
            SCOPED_TRACE(std::string(member) + " " + named);
            HostRig rig;
            json document = server_document();
            document[member] = named + "/endpoint";
            rig.host.answers[std::string(remote_issuer) + "/.well-known/oauth-authorization-server"] =
                json_result(200, document);
            const auto answer = rig.refuse();
            expect_refused_here(answer, rig.host, named);
            EXPECT_EQ(rig.host.fetched.size(), 2u);  // the two documents
        }
    }
}

TEST(OAuthSecurity, AChallengeElsewhereCannotNameMetadataOnThisMachine) {
    for (const std::string& named : this_machine) {
        SCOPED_TRACE(named);
        HostRig rig;
        const auto answer =
            rig.refuse({"Bearer resource_metadata=\"" + named + "/.well-known/oauth-protected-resource/mcp\""});
        expect_refused_here(answer, rig.host, named);
        EXPECT_TRUE(rig.host.fetched.empty());
    }
}

// Behind the URL guard, which keeps every request of the transports on the endpoint's origin, the gate itself gives a
// token only to a request of that origin.
TEST(OAuthSecurity, TheGateGivesATokenOnlyToARequestOfTheEndpointsOrigin) {
    const std::string own = "http://127.0.0.1:4100/mcp";
    auto authorizer = std::make_shared<auth_rig::ScriptedAuthorizer>("Bearer gate-token", 1);
    auto described = http_rig::endpoint(own);
    described.authorizer = authorizer;
    auto guard = neomcp::detail::http_client::UrlGuard::for_endpoint(own);
    ASSERT_TRUE(std::holds_alternative<neomcp::detail::http_client::UrlGuard>(guard));
    auto made = neomcp::detail::http_client::AuthorizationGate::make(
        described, std::get<neomcp::detail::http_client::UrlGuard>(guard));
    ASSERT_TRUE((std::holds_alternative<std::unique_ptr<neomcp::detail::http_client::AuthorizationGate>>(made)));
    auto& gate = *std::get<std::unique_ptr<neomcp::detail::http_client::AuthorizationGate>>(made);
    for (const char* other : {"http://127.0.0.1:4101/mcp", "https://127.0.0.1:4100/mcp", "http://localhost:4100/mcp",
                              "http://as.example/mcp", "not a url"}) {
        EXPECT_EQ(gate.credential({"POST", other, 1, nullptr}).value, "") << other;
    }
    EXPECT_EQ(authorizer->asked(), 0u);
    EXPECT_EQ(gate.credential({"POST", own + "?part=1", 1, nullptr}).value, "Bearer gate-token");
}

}  // namespace
