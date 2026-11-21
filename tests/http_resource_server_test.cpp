// Tests of the Streamable HTTP endpoint as an OAuth protected resource, over real sockets on the loopback interface,
// with the HTTP library's client as the peer: the metadata document, the challenges, the checks of every token, the
// verdicts remembered, the owner of a session, the bounds, and the end of a stream with its token.
//
// The validator is a table the test fills; a clock the test moves decides expiry. Every wait is on an event a test
// controls; the bound on a wait only limits how long a broken build takes to fail.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <httplib.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "http/endpoint_probe.h"
#include "neomcp/http/challenge.h"
#include "neomcp/http/endpoint.h"
#include "neomcp/http/listener.h"
#include "neomcp/http/resource_server.h"
#include "neomcp/http_server.h"
#include "neomcp/server.h"

namespace {

using namespace std::chrono_literals;
using neomcp::json;
using neomcp::http::TokenRefusal;
using neomcp::http::TokenVerdict;
using SystemClock = std::chrono::system_clock;

constexpr const char* path = "/mcp";
constexpr const char* document_path = "/.well-known/oauth-protected-resource/mcp";
constexpr const char* modern = "2026-07-28";
constexpr const char* legacy = "2025-11-25";
constexpr const char* json_type = "application/json";
constexpr const char* both_types = "application/json, text/event-stream";
constexpr const char* issuer_a = "https://auth.example";
constexpr const char* issuer_b = "https://other-auth.example";
constexpr const char* local_resource = "http://localhost/mcp";

// ---- The clock and the validator ------------------------------------------------------------------------------------

class FakeClock {
public:
    SystemClock::time_point now() const { return base_ + std::chrono::seconds(offset_.load()); }
    void advance(std::chrono::seconds by) { offset_ += by.count(); }
    std::function<SystemClock::time_point()> function() {
        return [this] { return now(); };
    }

private:
    const SystemClock::time_point base_ = SystemClock::now();
    std::atomic<long long> offset_{0};
};

// A validator from a table: a token it does not know is invalid. Counts its calls.
class Table final : public neomcp::http::TokenValidator {
public:
    void set(const std::string& token, TokenVerdict verdict) {
        std::lock_guard lock(mutex_);
        verdicts_.insert_or_assign(token, std::move(verdict));
    }
    void validate(const neomcp::http::TokenCheck& check, neomcp::http::TokenReply reply) override {
        TokenVerdict verdict = TokenRefusal::invalid;
        {
            std::lock_guard lock(mutex_);
            ++calls_;
            tokens_.emplace_back(check.token);
            resources_.emplace_back(check.resource);
            if (const auto found = verdicts_.find(std::string(check.token)); found != verdicts_.end()) {
                verdict = found->second;
            }
        }
        reply(std::move(verdict));
    }
    int calls() {
        std::lock_guard lock(mutex_);
        return calls_;
    }
    std::vector<std::string> tokens() {
        std::lock_guard lock(mutex_);
        return tokens_;
    }
    std::vector<std::string> resources() {
        std::lock_guard lock(mutex_);
        return resources_;
    }

private:
    std::mutex mutex_;
    std::map<std::string, TokenVerdict> verdicts_;
    int calls_ = 0;
    std::vector<std::string> tokens_;
    std::vector<std::string> resources_;
};

neomcp::Authorization granted(const FakeClock& clock, std::string subject, std::vector<std::string> scopes,
                              std::chrono::seconds lifetime = 3600s, std::string issuer = issuer_a,
                              std::vector<std::string> audiences = {local_resource}) {
    neomcp::Authorization authorization;
    authorization.issuer = std::move(issuer);
    authorization.subject = std::move(subject);
    authorization.client_id = "client-1";
    authorization.scopes = std::move(scopes);
    authorization.audiences = std::move(audiences);
    authorization.expires = clock.now() + lifetime;
    return authorization;
}

// ---- The server under test -------------------------------------------------------------------------------------------

void add_items(neomcp::Server& server) {
    server.tool("whoami", "The subject of the request's authorization", [](neomcp::Context& context) {
        const neomcp::Authorization* who = context.authorization();
        return who != nullptr && who->subject ? *who->subject : std::string("nobody");
    });
    server.tool("guarded", "A tool that needs files:write", [] { return std::string("written"); })
        .scopes({"files:write"});
    server.tool("audited", "A tool that needs audit:read", [] { return std::string("audited"); }).scopes({"audit:read"});
    server.resource("test://secret", "A resource that needs secret:read", [] { return std::string("s"); })
        .scopes({"secret:read"});
    server.resource("test://open", "A resource anyone may read", [] { return std::string("o"); });
}

neomcp::http::ResourceServerOptions resource_options(const std::shared_ptr<Table>& table, FakeClock& clock,
                                                     std::string resource = local_resource) {
    neomcp::http::ResourceServerOptions options;
    options.resource = std::move(resource);
    options.authorization_servers = {issuer_a};
    options.required_scopes = {"mcp:use"};
    options.scopes_supported = std::vector<std::string>{"mcp:use", "files:read", "files:write"};
    options.resource_name = "Test resource";
    options.validator = table;
    options.clock = clock.function();
    return options;
}

neomcp::HttpServerOptions quick_options() {
    neomcp::HttpServerOptions options;
    options.disconnect_poll_interval = 20ms;
    options.endpoint.stream_after = 1h;
    options.keep_alive_interval = 1h;
    options.worker_threads = 4;
    return options;
}

struct Harness {
    explicit Harness(std::function<void(neomcp::HttpServerOptions&)> tweak = {})
        : server(neomcp::ServerOptions{
              .name = "resource-server-test",
              .version = "0.0.1",
              .scope_implications = neomcp::ScopeImplications(
                  neomcp::ScopeImplications::Declared{{"admin", {"mcp:use", "files:write"}}})}) {
        add_items(server);
        neomcp::HttpServerOptions options = quick_options();
        options.authorization = resource_options(table, clock);
        if (tweak) tweak(options);
        http = std::make_unique<neomcp::HttpServer>(server, std::move(options));
        auto started = http->start();
        if (!started) throw std::runtime_error("start failed: " + started.error);
        port = *started.value;
    }

    // A token the table knows.
    std::string token(const std::string& name, neomcp::Authorization authorization) {
        table->set(name, std::move(authorization));
        return name;
    }

    FakeClock clock;
    std::shared_ptr<Table> table = std::make_shared<Table>();
    neomcp::Server server;
    std::unique_ptr<neomcp::HttpServer> http;
    std::uint16_t port = 0;
};

// ---- The client ------------------------------------------------------------------------------------------------------

struct Reply {
    bool ok = false;
    int status = 0;
    httplib::Headers headers;
    std::string body;

    std::string header(const std::string& name) const {
        const auto found = headers.find(name);
        return found == headers.end() ? std::string() : found->second;
    }
    std::size_t count(const std::string& name) const { return headers.count(name); }
};

Reply send(std::uint16_t port, const std::string& method, httplib::Headers headers, std::string body = {},
           const std::string& target = path) {
    httplib::Client client("127.0.0.1", port);
    client.set_keep_alive(false);
    client.set_connection_timeout(30, 0);
    client.set_read_timeout(30, 0);
    httplib::Request request;
    request.method = method;
    request.path = target;
    request.headers = std::move(headers);
    request.body = std::move(body);
    Reply reply;
    if (auto result = client.send(request)) {
        reply.ok = true;
        reply.status = result->status;
        reply.headers = result->headers;
        reply.body = result->body;
    }
    return reply;
}

std::string rpc(const json& id, const std::string& method, json params = json::object()) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}}.dump();
}

json modern_params(json params = json::object()) {
    params["_meta"] = json{{"io.modelcontextprotocol/protocolVersion", modern},
                           {"io.modelcontextprotocol/clientInfo", {{"name", "test-client"}, {"version", "1.0.0"}}},
                           {"io.modelcontextprotocol/clientCapabilities", json::object()}};
    return params;
}

httplib::Headers modern_headers(const std::string& method, const std::optional<std::string>& token,
                                std::optional<std::string> name = std::nullopt) {
    httplib::Headers headers{{"Content-Type", json_type},
                             {"Accept", both_types},
                             {"MCP-Protocol-Version", modern},
                             {"Mcp-Method", method}};
    if (name) headers.emplace("Mcp-Name", *name);
    if (token) headers.emplace("Authorization", "Bearer " + *token);
    return headers;
}

Reply call_tool(std::uint16_t port, const std::optional<std::string>& token, const std::string& tool) {
    return send(port, "POST", modern_headers("tools/call", token, tool),
                rpc(1, "tools/call", modern_params({{"name", tool}, {"arguments", json::object()}})));
}

Reply list_tools(std::uint16_t port, const std::optional<std::string>& token) {
    return send(port, "POST", modern_headers("tools/list", token), rpc(1, "tools/list", modern_params()));
}

httplib::Headers legacy_headers(const std::string& session, const std::string& token) {
    httplib::Headers headers{{"Content-Type", json_type},
                             {"Accept", both_types},
                             {"MCP-Protocol-Version", legacy},
                             {"Authorization", "Bearer " + token}};
    if (!session.empty()) headers.emplace("Mcp-Session-Id", session);
    return headers;
}

// A legacy session opened with `token`; its id.
std::string open_session(std::uint16_t port, const std::string& token) {
    const json params{{"protocolVersion", legacy},
                      {"capabilities", json::object()},
                      {"clientInfo", {{"name", "test-client"}, {"version", "1.0.0"}}}};
    const Reply opened = send(port, "POST", legacy_headers({}, token), rpc(1, "initialize", params));
    if (opened.status != 200) throw std::runtime_error("initialize answered " + std::to_string(opened.status));
    const std::string id = opened.header("Mcp-Session-Id");
    const Reply ready = send(port, "POST", legacy_headers(id, token),
                             json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}.dump());
    if (ready.status != 202) throw std::runtime_error("initialized answered " + std::to_string(ready.status));
    return id;
}

Reply ping(std::uint16_t port, const std::string& session, const std::string& token) {
    return send(port, "POST", legacy_headers(session, token), rpc(2, "ping"));
}

// The Bearer challenge of a reply.
neomcp::http::BearerChallenge challenge_of(const Reply& reply) {
    std::vector<std::string> values;
    for (auto [at, end] = reply.headers.equal_range("WWW-Authenticate"); at != end; ++at) values.push_back(at->second);
    auto parsed = neomcp::http::parse_challenges(std::span<const std::string>(values));
    const auto* challenges = std::get_if<std::vector<neomcp::http::Challenge>>(&parsed);
    if (challenges == nullptr) throw std::runtime_error("the challenge cannot be read");
    auto bearer = neomcp::http::find_bearer_challenge(*challenges);
    const auto* found = std::get_if<neomcp::http::BearerChallenge>(&bearer);
    if (found == nullptr) throw std::runtime_error("no Bearer challenge");
    return *found;
}

std::string error_of(const Reply& reply) { return json::parse(reply.body).at("error").get<std::string>(); }

std::string text_of(const Reply& reply) {
    return json::parse(reply.body).at("result").at("content").at(0).at("text").get<std::string>();
}

// ---- The metadata document --------------------------------------------------------------------------------------------

// The endpoint on this library's listener, with the resource identifier the bound port makes, and a view of what it
// gives its sessions. The listener's handlers reach the endpoint, made once the port is known, through a forwarder.
struct ProbeRig {
    struct Forward {
        std::mutex mutex;
        neomcp::http::Handler main;
        neomcp::http::Handler document;
        void call(bool for_document, neomcp::http::Request request, neomcp::http::Responder responder) {
            neomcp::http::Handler handler;
            {
                std::lock_guard lock(mutex);
                handler = for_document ? document : main;
            }
            if (handler) {
                handler(std::move(request), std::move(responder));
            } else {
                responder.complete(neomcp::http::make_response(503));
            }
        }
    };

    explicit ProbeRig(neomcp::http::detail::EndpointProbe::FeedObserver observer = {})
        : server(neomcp::ServerOptions{.name = "probe-test", .version = "0.0.1"}) {
        add_items(server);
        neomcp::http::ListenerOptions listening;
        listening.path = path;
        listening.disconnect_poll_interval = 20ms;
        listening.keep_alive_interval = 1h;
        listening.routes.push_back(neomcp::http::Route{
            document_path, [forward = forward](neomcp::http::Request request, neomcp::http::Responder responder) {
                forward->call(true, std::move(request), std::move(responder));
            }});
        listener = std::make_unique<neomcp::http::Listener>(
            std::move(listening), [forward = forward](neomcp::http::Request request, neomcp::http::Responder responder) {
                forward->call(false, std::move(request), std::move(responder));
            });
        auto bound = listener->start();
        if (!bound) throw std::runtime_error("listen failed: " + bound.error);
        port = *bound.value;
        resource = "http://127.0.0.1:" + std::to_string(port) + path;
        neomcp::http::PolicyOptions policy_options;
        policy_options.bound_port = port;
        auto policy = neomcp::http::HostPolicy::create(policy_options);
        if (!policy) throw std::runtime_error(policy.error);
        neomcp::http::EndpointOptions options;
        options.stream_after = 1h;
        options.authorization = resource_options(table, clock, resource);
        options.authorization->authorization_servers = {issuer_a, issuer_b};
        auto made = neomcp::http::detail::EndpointProbe::create(server, std::move(*policy.value), std::move(options),
                                                                std::move(observer));
        if (!made) throw std::runtime_error(made.error);
        endpoint = std::move(*made.value);
        std::lock_guard lock(forward->mutex);
        forward->main = endpoint->handler();
        forward->document = endpoint->metadata_handler();
    }
    ~ProbeRig() {
        endpoint->stop();
        listener.reset();
        endpoint.reset();
    }
    ProbeRig(const ProbeRig&) = delete;
    ProbeRig& operator=(const ProbeRig&) = delete;

    std::string token(const std::string& name, const std::string& subject, std::vector<std::string> scopes) {
        table->set(name, granted(clock, subject, std::move(scopes), 3600s, issuer_a, {resource}));
        return name;
    }

    FakeClock clock;
    std::shared_ptr<Table> table = std::make_shared<Table>();
    std::shared_ptr<Forward> forward = std::make_shared<Forward>();
    neomcp::Server server;
    std::unique_ptr<neomcp::http::Listener> listener;
    std::unique_ptr<neomcp::http::Endpoint> endpoint;
    std::uint16_t port = 0;
    std::string resource;
};

TEST(ResourceServerMetadata, IsServedOnTheWellKnownPathWithTheResourceVerbatimAndNoToken) {
    ProbeRig rig;
    const std::string url = "http://127.0.0.1:" + std::to_string(rig.port) + document_path;
    // Whatever the request says about where it was sent, the document and the challenges name the configured URLs.
    const Reply document = send(rig.port, "GET", {{"X-Forwarded-Host", "elsewhere.example"}}, {}, document_path);
    ASSERT_EQ(document.status, 200);
    EXPECT_EQ(document.header("Content-Type"), json_type);
    EXPECT_EQ(document.header("Cache-Control"), "max-age=300");
    const json body = json::parse(document.body);
    EXPECT_EQ(body.at("resource"), rig.resource);
    EXPECT_EQ(body.at("authorization_servers"), (json{issuer_a, issuer_b}));
    EXPECT_EQ(body.at("scopes_supported"), (json{"mcp:use", "files:read", "files:write"}));
    EXPECT_EQ(body.at("bearer_methods_supported"), (json{"header"}));
    EXPECT_EQ(body.at("resource_name"), "Test resource");
    EXPECT_EQ(rig.table->calls(), 0);

    const Reply head = send(rig.port, "HEAD", {}, {}, document_path);
    EXPECT_EQ(head.status, 200);
    EXPECT_TRUE(head.body.empty());
    const Reply post = send(rig.port, "POST", {{"Content-Type", json_type}}, "{}", document_path);
    EXPECT_EQ(post.status, 405);
    EXPECT_EQ(post.header("Allow"), "GET, HEAD");
    // The Host and Origin policy applies as on the endpoint.
    EXPECT_EQ(send(rig.port, "GET", {{"Host", "evil.example"}}, {}, document_path).status, 403);
    EXPECT_EQ(send(rig.port, "GET", {{"Origin", "https://evil.example"}}, {}, document_path).status, 403);

    const Reply refused = send(rig.port, "POST", modern_headers("tools/list", std::nullopt),
                               rpc(1, "tools/list", modern_params()));
    ASSERT_EQ(refused.status, 401);
    EXPECT_EQ(challenge_of(refused).resource_metadata, url);
}

TEST(ResourceServerMetadata, LeavesOutScopesSupportedWhenThereAreNoneAndChangesWithTheAuthorizationServers) {
    Harness harness([](neomcp::HttpServerOptions& options) {
        options.authorization->required_scopes.clear();
        options.authorization->scopes_supported.reset();
        options.authorization->resource_name.reset();
    });
    const auto fetch = [&] { return json::parse(send(harness.port, "GET", {}, {}, document_path).body); };
    const json before = fetch();
    EXPECT_FALSE(before.contains("scopes_supported"));
    EXPECT_FALSE(before.contains("resource_name"));
    EXPECT_EQ(before.at("resource"), local_resource);

    const std::string token = harness.token("t", granted(harness.clock, "alice", {}));
    EXPECT_EQ(list_tools(harness.port, token).status, 200);
    ASSERT_TRUE(harness.http->update_authorization_servers({issuer_b}));
    EXPECT_EQ(fetch().at("authorization_servers"), (json{issuer_b}));
    // The verdict on the token is remembered, and its issuer is no longer accepted: refused from now on.
    const Reply refused = list_tools(harness.port, token);
    EXPECT_EQ(refused.status, 401);
    EXPECT_EQ(challenge_of(refused).error, "invalid_token");
    EXPECT_EQ(harness.table->calls(), 1);
    EXPECT_FALSE(harness.http->update_authorization_servers({}));
    EXPECT_FALSE(harness.http->update_authorization_servers({"http://not-loopback.example"}));
}

// ---- Challenges --------------------------------------------------------------------------------------------------

TEST(ResourceServerGate, ARequestWithoutBearerCredentialsGets401WithTheChallengeAndNoBody) {
    Harness harness;
    for (const std::optional<std::string>& field :
         {std::optional<std::string>(), std::optional<std::string>("Basic dXNlcjpwYXNz")}) {
        httplib::Headers headers = modern_headers("tools/list", std::nullopt);
        if (field) headers.emplace("Authorization", *field);
        const Reply reply = send(harness.port, "POST", std::move(headers), rpc(1, "tools/list", modern_params()));
        EXPECT_EQ(reply.status, 401);
        EXPECT_TRUE(reply.body.empty());
        EXPECT_EQ(reply.header("Cache-Control"), "no-store");
        const auto bearer = challenge_of(reply);
        EXPECT_EQ(bearer.resource_metadata, "http://localhost/.well-known/oauth-protected-resource/mcp");
        EXPECT_EQ(bearer.scope, "mcp:use");
        EXPECT_FALSE(bearer.error.has_value());
    }
    // A token anywhere but the Authorization field is not read.
    const Reply in_query =
        send(harness.port, "POST", modern_headers("tools/list", std::nullopt), rpc(1, "tools/list", modern_params()),
             std::string(path) + "?access_token=t");
    EXPECT_EQ(in_query.status, 401);
    EXPECT_EQ(harness.table->calls(), 0);
}

TEST(ResourceServerGate, CredentialsThatCannotBeReadGet400InvalidRequest) {
    Harness harness;
    const std::string token = harness.token("t", granted(harness.clock, "alice", {"mcp:use"}));
    const auto with_fields = [&](std::vector<std::string> values) {
        httplib::Headers headers = modern_headers("tools/list", std::nullopt);
        for (std::string& value : values) headers.emplace("Authorization", std::move(value));
        return send(harness.port, "POST", std::move(headers), rpc(1, "tools/list", modern_params()));
    };
    for (const auto& values : std::vector<std::vector<std::string>>{
             {"Bearer t", "Bearer t"}, {"Bearer"}, {"Bearer t u"}, {"Bearer t,"}, {"Bearer \"t\""}}) {
        const Reply reply = with_fields(values);
        EXPECT_EQ(reply.status, 400) << values.front();
        EXPECT_EQ(challenge_of(reply).error, "invalid_request");
        EXPECT_EQ(error_of(reply), "invalid_request");
        EXPECT_EQ(reply.header("Cache-Control"), "no-store");
    }
    EXPECT_EQ(harness.table->calls(), 0);
    EXPECT_EQ(with_fields({"bearer   t"}).status, 200) << "the scheme without case, and more than one space";
}

TEST(ResourceServerGate, TokensTheChecksRefuseGet401InvalidTokenWhateverTheValidatorSaid) {
    Harness harness;
    auto& clock = harness.clock;
    auto expired = granted(clock, "a", {"mcp:use"}, -61s);  // past the leeway of 60 s
    auto early = granted(clock, "a", {"mcp:use"});
    early.not_before = clock.now() + 61s;
    auto without_expiry = granted(clock, "a", {"mcp:use"});
    without_expiry.expires.reset();
    auto without_issuer = granted(clock, "a", {"mcp:use"});
    without_issuer.issuer.clear();
    const std::map<std::string, neomcp::Authorization> refused{
        {"unknown-issuer", granted(clock, "a", {"mcp:use"}, 3600s, issuer_b)},
        {"near-issuer", granted(clock, "a", {"mcp:use"}, 3600s, std::string(issuer_a) + "/")},
        {"other-audience", granted(clock, "a", {"mcp:use"}, 3600s, issuer_a, {"http://localhost/other"})},
        {"near-audience", granted(clock, "a", {"mcp:use"}, 3600s, issuer_a, {"http://localhost/mcp/"})},
        {"no-audience", granted(clock, "a", {"mcp:use"}, 3600s, issuer_a, {})},
        {"expired", expired},
        {"early", early},
        {"no-expiry", without_expiry},
        {"no-issuer", without_issuer},
    };
    for (const auto& [name, authorization] : refused) harness.token(name, authorization);
    for (const auto& item : refused) {
        const Reply reply = list_tools(harness.port, item.first);
        EXPECT_EQ(reply.status, 401) << item.first;
        const auto bearer = challenge_of(reply);
        EXPECT_EQ(bearer.error, "invalid_token") << item.first;
        EXPECT_EQ(bearer.scope, "mcp:use");
        EXPECT_EQ(error_of(reply), "invalid_token");
    }
    EXPECT_EQ(list_tools(harness.port, "never-issued").status, 401);
    // At the edges of the leeway: one second inside passes.
    harness.token("just-alive", granted(clock, "a", {"mcp:use"}, -59s));
    auto just_valid = granted(clock, "a", {"mcp:use"});
    just_valid.not_before = clock.now() + 59s;
    harness.token("just-valid", just_valid);
    EXPECT_EQ(list_tools(harness.port, "just-alive").status, 200);
    EXPECT_EQ(list_tools(harness.port, "just-valid").status, 200);
    for (const std::string& resource : harness.table->resources()) EXPECT_EQ(resource, local_resource);
}

TEST(ResourceServerGate, AValidTokenReachesTheHandlerAndTheFieldNeverReachesTheSession) {
    struct Seen {
        std::mutex mutex;
        int feeds = 0;
        int with_field = 0;
        int with_token = 0;
        int without_authorization = 0;
    };
    auto seen = std::make_shared<Seen>();
    const std::string planted = "planted-access-token-0123456789";
    ProbeRig rig([seen, planted](std::span<const neomcp::HeaderField> headers, const neomcp::Authorization* given) {
        std::lock_guard lock(seen->mutex);
        ++seen->feeds;
        for (const neomcp::HeaderField& field : headers) {
            if (neomcp::http::equal_ignoring_case(field.name, "Authorization")) ++seen->with_field;
            if (field.value.find(planted) != std::string_view::npos) ++seen->with_token;
        }
        if (given == nullptr) ++seen->without_authorization;
    });
    rig.token(planted, "alice", {"mcp:use"});
    const Reply reply = call_tool(rig.port, planted, "whoami");
    ASSERT_EQ(reply.status, 200) << reply.body;
    EXPECT_EQ(text_of(reply), "alice");
    EXPECT_EQ(reply.body.find(planted), std::string::npos);
    std::lock_guard lock(seen->mutex);
    EXPECT_EQ(seen->feeds, 1);
    EXPECT_EQ(seen->with_field, 0);
    EXPECT_EQ(seen->with_token, 0);
    EXPECT_EQ(seen->without_authorization, 0);
}

TEST(ResourceServerGate, ATokenWithoutTheRequiredScopesGets403AndADeclaredImplicationMeetsThem) {
    Harness harness;
    const std::string lacking = harness.token("lacking", granted(harness.clock, "a", {"files:read", "openid"}));
    const Reply reply = list_tools(harness.port, lacking);
    EXPECT_EQ(reply.status, 403);
    const auto bearer = challenge_of(reply);
    EXPECT_EQ(bearer.error, "insufficient_scope");
    EXPECT_EQ(bearer.scope, "mcp:use files:read") << "what it lacks, then the scopes it holds that the server names";
    EXPECT_EQ(error_of(reply), "insufficient_scope");
    EXPECT_EQ(reply.header("Cache-Control"), "no-store");
    const std::string admin = harness.token("admin", granted(harness.clock, "a", {"admin"}));
    EXPECT_EQ(list_tools(harness.port, admin).status, 200);
}

TEST(ResourceServerGate, AToolsDeclaredScopeIsAskedForWithA403ThatNamesWhatItLacksAndWhatTheServerNames) {
    Harness harness;
    const std::string reader = harness.token("reader", granted(harness.clock, "a", {"mcp:use", "files:read", "openid"}));
    const Reply reply = call_tool(harness.port, reader, "guarded");
    EXPECT_EQ(reply.status, 403) << reply.body;
    const auto bearer = challenge_of(reply);
    EXPECT_EQ(bearer.error, "insufficient_scope");
    EXPECT_EQ(bearer.scope, "files:write mcp:use files:read") << "openid is not the server's to name";
    EXPECT_EQ(error_of(reply), "insufficient_scope");
    EXPECT_EQ(reply.header("Cache-Control"), "no-store");
    const std::string writer = harness.token("writer", granted(harness.clock, "a", {"mcp:use", "files:write"}));
    EXPECT_EQ(text_of(call_tool(harness.port, writer, "guarded")), "written");
}

TEST(ResourceServerGate, ABatchIsAskedForTheScopesEveryRequestOfItLacksInOneChallenge) {
    Harness harness;
    const std::string token = harness.token("t", granted(harness.clock, "a", {"mcp:use"}));
    const char* batching = "2025-03-26";
    const json params{{"protocolVersion", batching},
                      {"capabilities", json::object()},
                      {"clientInfo", {{"name", "test-client"}, {"version", "1.0.0"}}}};
    httplib::Headers headers{{"Content-Type", json_type}, {"Accept", both_types}, {"Authorization", "Bearer " + token}};
    const Reply opened = send(harness.port, "POST", headers, rpc(1, "initialize", params));
    ASSERT_EQ(opened.status, 200) << opened.body;
    headers.emplace("Mcp-Session-Id", opened.header("Mcp-Session-Id"));
    ASSERT_EQ(send(harness.port, "POST", headers, json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}.dump())
                  .status,
              202);
    json batch = json::array();
    for (const char* tool : {"guarded", "whoami", "audited"}) {
        batch.push_back(json{{"jsonrpc", "2.0"},
                             {"id", tool},
                             {"method", "tools/call"},
                             {"params", {{"name", tool}, {"arguments", json::object()}}}});
    }
    const Reply refused = send(harness.port, "POST", headers, batch.dump());
    EXPECT_EQ(refused.status, 403) << refused.body;
    EXPECT_EQ(challenge_of(refused).scope, "files:write audit:read mcp:use");
    EXPECT_EQ(error_of(refused), "insufficient_scope");
}

TEST(ResourceServerGate, ASubscriptionThatNamesAResourceItCannotReadIsRefused) {
    Harness harness;
    const std::string token = harness.token("t", granted(harness.clock, "a", {"mcp:use"}));
    const auto subscribe = [&](json filter) {
        return send(harness.port, "POST", modern_headers("subscriptions/listen", token),
                    rpc(5, "subscriptions/listen", modern_params({{"notifications", std::move(filter)}})));
    };
    const Reply refused = subscribe({{"resourceSubscriptions", {"test://open", "test://secret"}}});
    EXPECT_EQ(refused.status, 403) << refused.body;
    EXPECT_EQ(challenge_of(refused).scope, "secret:read mcp:use");
}

// ---- What the validator cannot answer ---------------------------------------------------------------------------

class Unanswering final : public neomcp::http::TokenValidator {
public:
    enum class Way { unavailable, never, drop, throws };
    explicit Unanswering(Way way) : way_(way) {}
    void validate(const neomcp::http::TokenCheck&, neomcp::http::TokenReply reply) override {
        calls.fetch_add(1);
        switch (way_) {
            case Way::unavailable: reply(TokenRefusal::unavailable); return;
            case Way::never: {
                std::lock_guard lock(mutex);
                kept.push_back(std::move(reply));
                return;
            }
            case Way::drop: return;
            case Way::throws: throw std::runtime_error("the validator failed");
        }
    }
    std::atomic<int> calls{0};
    std::mutex mutex;
    std::vector<neomcp::http::TokenReply> kept;

private:
    const Way way_;
};

TEST(ResourceServerGate, AValidatorThatCannotAnswerGives503AndNothingIsRemembered) {
    for (const auto way : {Unanswering::Way::unavailable, Unanswering::Way::never, Unanswering::Way::drop,
                           Unanswering::Way::throws}) {
        auto validator = std::make_shared<Unanswering>(way);
        Harness harness([&](neomcp::HttpServerOptions& options) {
            options.authorization->validator = validator;
            // Only the validator that never answers waits for it.
            options.authorization->bounds.validation_timeout =
                way == Unanswering::Way::never ? std::chrono::milliseconds(200) : std::chrono::milliseconds(3600000);
        });
        const auto started = std::chrono::steady_clock::now();
        const Reply first = list_tools(harness.port, "t");
        EXPECT_EQ(first.status, 503) << static_cast<int>(way);
        EXPECT_FALSE(first.headers.count("WWW-Authenticate")) << "not the token's fault";
        EXPECT_EQ(first.header("Cache-Control"), "no-store") << static_cast<int>(way);
        EXPECT_LT(std::chrono::steady_clock::now() - started, 20s);
        EXPECT_EQ(list_tools(harness.port, "t").status, 503);
        EXPECT_EQ(validator->calls.load(), 2) << static_cast<int>(way);
        std::lock_guard lock(validator->mutex);
        for (auto& reply : validator->kept) reply(neomcp::Authorization{});  // too late: dropped
    }
}

TEST(ResourceServerGate, OneTokenTooManyInValidationGets503) {
    auto validator = std::make_shared<Unanswering>(Unanswering::Way::never);
    Harness harness([&](neomcp::HttpServerOptions& options) {
        options.authorization->validator = validator;
        options.authorization->bounds.max_pending_validations = 1;
        options.authorization->bounds.validation_timeout = 5s;
    });
    std::thread waiting([&] { EXPECT_EQ(list_tools(harness.port, "first").status, 503); });
    while (validator->calls.load() == 0) std::this_thread::yield();
    const auto started = std::chrono::steady_clock::now();
    const Reply second = list_tools(harness.port, "second");
    EXPECT_EQ(second.status, 503);
    EXPECT_EQ(second.header("Cache-Control"), "no-store");
    EXPECT_LT(std::chrono::steady_clock::now() - started, 4s) << "refused at once, not after the timeout";
    EXPECT_EQ(validator->calls.load(), 1);
    waiting.join();
}

// ---- Remembered verdicts ----------------------------------------------------------------------------------------

TEST(ResourceServerCache, ATokenIsValidatedOnceForManyRequests) {
    Harness harness;
    const std::string token = harness.token("t", granted(harness.clock, "a", {"mcp:use"}));
    for (int i = 0; i < 5; ++i) EXPECT_EQ(list_tools(harness.port, token).status, 200);
    EXPECT_EQ(harness.table->calls(), 1);
}

TEST(ResourceServerCache, ARevokedTokenPassesUntilMaxCacheAgeThenIsRefused) {
    Harness harness;
    const std::string token = harness.token("t", granted(harness.clock, "a", {"mcp:use"}));
    EXPECT_EQ(list_tools(harness.port, token).status, 200);
    harness.table->set(token, TokenRefusal::invalid);
    harness.clock.advance(59s);
    EXPECT_EQ(list_tools(harness.port, token).status, 200) << "remembered for 60 s";
    harness.clock.advance(1s);
    EXPECT_EQ(list_tools(harness.port, token).status, 401);
    EXPECT_EQ(harness.table->calls(), 2);
}

TEST(ResourceServerCache, AVerdictIsNeverRememberedPastTheTokensExpiry) {
    Harness harness([](neomcp::HttpServerOptions& options) { options.authorization->bounds.leeway = 0s; });
    const std::string token = harness.token("t", granted(harness.clock, "a", {"mcp:use"}, 10s));
    EXPECT_EQ(list_tools(harness.port, token).status, 200);
    harness.clock.advance(10s);
    EXPECT_EQ(list_tools(harness.port, token).status, 401);
    EXPECT_EQ(harness.table->calls(), 2) << "the token was asked about again once it expired";
}

TEST(ResourceServerCache, AnInvalidVerdictIsRememberedForTheNegativeAge) {
    Harness harness;
    EXPECT_EQ(list_tools(harness.port, "bad").status, 401);
    harness.clock.advance(4s);
    EXPECT_EQ(list_tools(harness.port, "bad").status, 401);
    EXPECT_EQ(harness.table->calls(), 1);
    harness.clock.advance(1s);
    EXPECT_EQ(list_tools(harness.port, "bad").status, 401);
    EXPECT_EQ(harness.table->calls(), 2);
}

// ---- The owner of a session -------------------------------------------------------------------------------------

TEST(ResourceServerSession, AnotherPrincipalNamingASessionGets404AndTheOwnerGoesOnWithANewToken) {
    Harness harness;
    const std::string alice = harness.token("alice-1", granted(harness.clock, "alice", {"mcp:use"}));
    const std::string bob = harness.token("bob", granted(harness.clock, "bob", {"mcp:use"}));
    const std::string session = open_session(harness.port, alice);
    EXPECT_EQ(ping(harness.port, session, bob).status, 404);
    EXPECT_EQ(send(harness.port, "GET", legacy_headers(session, bob)).status, 404);
    EXPECT_EQ(send(harness.port, "DELETE", legacy_headers(session, bob)).status, 404);
    const std::string refreshed = harness.token("alice-2", granted(harness.clock, "alice", {"mcp:use", "files:read"}));
    EXPECT_EQ(ping(harness.port, session, refreshed).status, 200);
    EXPECT_EQ(send(harness.port, "DELETE", legacy_headers(session, refreshed)).status, 204);
}

// ---- Bounds -----------------------------------------------------------------------------------------------------

TEST(ResourceServerBounds, ATokenAtTheDefaultBoundReachesTheValidatorThroughTheListener) {
    Harness harness;
    const std::string longest(neomcp::http::ResourceServerDefaults::max_token_bytes, 'a');
    harness.token(longest, granted(harness.clock, "a", {"mcp:use"}));
    EXPECT_EQ(list_tools(harness.port, longest).status, 200);
    ASSERT_EQ(harness.table->tokens().size(), 1u);
    EXPECT_EQ(harness.table->tokens().front().size(), longest.size());
    // One byte more does not fit the header line the listener reads: it refuses the request before the endpoint.
    const Reply beyond = list_tools(harness.port, longest + "a");
    EXPECT_EQ(beyond.status, 400);
    EXPECT_EQ(harness.table->calls(), 1);
}

TEST(ResourceServerBounds, ATokenOnePastAConfiguredBoundGets401WithoutValidation) {
    Harness harness([](neomcp::HttpServerOptions& options) { options.authorization->bounds.max_token_bytes = 32; });
    const std::string fits(32, 'b');
    harness.token(fits, granted(harness.clock, "a", {"mcp:use"}));
    EXPECT_EQ(list_tools(harness.port, fits).status, 200);
    const Reply beyond = list_tools(harness.port, fits + "b");
    EXPECT_EQ(beyond.status, 401);
    EXPECT_EQ(challenge_of(beyond).error, "invalid_token");
    EXPECT_EQ(harness.table->calls(), 1);
}

// ---- Streams end with their token -------------------------------------------------------------------------------

TEST(ResourceServerStreams, TheStreamOfASessionEndsWhenTheTokenThatOpenedItExpires) {
    Harness harness([](neomcp::HttpServerOptions& options) { options.authorization->bounds.leeway = 0s; });
    const std::string opener = harness.token("long", granted(harness.clock, "alice", {"mcp:use"}));
    const std::string session = open_session(harness.port, opener);
    const std::string brief = harness.token("brief", granted(harness.clock, "alice", {"mcp:use"}, 1s));
    httplib::Headers headers = legacy_headers(session, brief);
    headers.erase("Accept");
    headers.emplace("Accept", "text/event-stream");
    const auto started = std::chrono::steady_clock::now();
    const Reply stream = send(harness.port, "GET", std::move(headers));
    EXPECT_TRUE(stream.ok) << "ended by the server, not by the client's read timeout";
    EXPECT_EQ(stream.status, 200);
    EXPECT_GE(std::chrono::steady_clock::now() - started, 500ms);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 20s);
    EXPECT_EQ(ping(harness.port, session, opener).status, 200) << "the session goes on; only its stream ended";
    // The client opens the stream again with its current token: the ended one no longer holds the session's place.
    const std::string next = harness.token("next", granted(harness.clock, "alice", {"mcp:use"}, 1s));
    httplib::Headers again = legacy_headers(session, next);
    again.erase("Accept");
    again.emplace("Accept", "text/event-stream");
    const Reply reopened = send(harness.port, "GET", std::move(again));
    EXPECT_TRUE(reopened.ok);
    EXPECT_EQ(reopened.status, 200) << "not 409: the expired stream was let go";
}

TEST(ResourceServerStreams, ASubscriptionEndsWhenTheTokenThatOpenedItExpires) {
    Harness harness([](neomcp::HttpServerOptions& options) { options.authorization->bounds.leeway = 0s; });
    const std::string brief = harness.token("brief", granted(harness.clock, "alice", {"mcp:use"}, 1s));
    const auto started = std::chrono::steady_clock::now();
    const Reply stream =
        send(harness.port, "POST", modern_headers("subscriptions/listen", brief),
             rpc(5, "subscriptions/listen", modern_params({{"notifications", {{"toolsListChanged", true}}}})));
    EXPECT_TRUE(stream.ok);
    EXPECT_EQ(stream.status, 200);
    EXPECT_EQ(stream.header("Content-Type"), "text/event-stream");
    EXPECT_NE(stream.body.find("notifications/subscriptions/acknowledged"), std::string::npos);
    EXPECT_GE(std::chrono::steady_clock::now() - started, 500ms);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 20s);
}

// ---- Starting -----------------------------------------------------------------------------------------------------

TEST(ResourceServerStart, RefusesOptionsThatCannotBeServed) {
    FakeClock clock;
    auto table = std::make_shared<Table>();
    const std::vector<std::function<void(neomcp::http::ResourceServerOptions&)>> broken{
        [](auto& o) { o.validator.reset(); },
        [](auto& o) { o.resource = "ftp://localhost/mcp"; },
        [](auto& o) { o.resource = "http://example.com/mcp"; },
        [](auto& o) { o.resource = "http://localhost/mcp?x=1"; },
        [](auto& o) { o.resource = "http://localhost/mcp#part"; },
        [](auto& o) { o.resource = "http://user@localhost/mcp"; },
        [](auto& o) { o.resource = "/mcp"; },
        [](auto& o) { o.authorization_servers.clear(); },
        [](auto& o) { o.authorization_servers = std::vector<std::string>(17, issuer_a); },
        [](auto& o) { o.authorization_servers = {"https://auth.example?tenant=1"}; },
        [](auto& o) { o.required_scopes = {"offline_access"}; },
        [](auto& o) { o.required_scopes = {"two words"}; },
        [](auto& o) { o.scopes_supported = std::vector<std::string>{"offline_access"}; },
        [](auto& o) { o.accepted_audiences = {""}; },
        [](auto& o) { o.bounds.leeway = 301s; },
        [](auto& o) { o.bounds.max_token_bytes = 0; },
        [](auto& o) { o.bounds.validation_timeout = 0ms; },
    };
    for (std::size_t at = 0; at < broken.size(); ++at) {
        neomcp::Server server({.name = "s", .version = "1"});
        neomcp::HttpServerOptions options = quick_options();
        options.authorization = resource_options(table, clock);
        broken[at](*options.authorization);
        neomcp::HttpServer http(server, std::move(options));
        EXPECT_FALSE(http.start()) << at;
    }
    neomcp::http::ResourceServerOptions edge = resource_options(table, clock);
    edge.bounds.leeway = 300s;
    neomcp::Server server({.name = "s", .version = "1"});
    neomcp::HttpServerOptions options = quick_options();
    options.authorization = edge;
    neomcp::HttpServer http(server, std::move(options));
    EXPECT_TRUE(http.start());
}

TEST(ResourceServerStart, RefusesWhatTheServerCannotProtectOrTheListenerCannotCarry) {
    FakeClock clock;
    auto table = std::make_shared<Table>();
    const auto starts = [&](const std::function<void(neomcp::HttpServerOptions&)>& tweak, bool scoped) {
        neomcp::Server server({.name = "s", .version = "1"});
        if (scoped) server.tool("guarded", "g", [] { return std::string("g"); }).scopes({"files:write"});
        neomcp::HttpServerOptions options = quick_options();
        tweak(options);
        neomcp::HttpServer http(server, std::move(options));
        return static_cast<bool>(http.start());
    };
    EXPECT_FALSE(starts([](auto&) {}, true)) << "declared scopes need authorization";
    EXPECT_TRUE(starts([](auto&) {}, false));
    EXPECT_TRUE(starts([&](auto& o) { o.authorization = resource_options(table, clock); }, true));
    EXPECT_FALSE(starts(
        [&](auto& o) {
            o.authorization = resource_options(table, clock);
            o.endpoint.authorization = resource_options(table, clock);
        },
        false));
    EXPECT_FALSE(starts(
        [&](auto& o) {
            o.authorization = resource_options(table, clock);
            o.authorization->bounds.max_token_bytes = neomcp::http::ResourceServerDefaults::max_token_bytes + 1;
        },
        false));
    EXPECT_FALSE(starts([&](auto& o) { o.authorization = resource_options(table, clock, "https://example.com/mcp"); },
                        false))
        << "the Host check would refuse every request to that host";
    EXPECT_TRUE(starts(
        [&](auto& o) {
            o.authorization = resource_options(table, clock, "https://example.com/mcp");
            o.allowed_hosts = {"example.com"};
        },
        false));
}

}  // namespace
