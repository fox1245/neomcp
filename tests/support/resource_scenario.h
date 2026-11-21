// What the socket scenarios of the Streamable HTTP endpoint as an OAuth protected resource share: a clock the test
// moves, a validator from a table, a server with items that declare scopes (and count how often they ran), a request
// sender, and a request that is read as it arrives. Header only; every name lives in neomcp::scenario.
#pragma once

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <httplib.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/describe.h"
#include "neomcp/http/challenge.h"
#include "neomcp/http/resource_server.h"
#include "neomcp/http_server.h"
#include "neomcp/server.h"

namespace neomcp::scenario {

struct DocumentArgs {
    std::string name;
};

struct TopicArgs {
    std::string topic;
};

}  // namespace neomcp::scenario

NEOMCP_DESCRIBE(neomcp::scenario::DocumentArgs, name, "the document")
NEOMCP_DESCRIBE(neomcp::scenario::TopicArgs, topic, "what to plan")

namespace neomcp::scenario {

using namespace std::chrono_literals;
using SystemClock = std::chrono::system_clock;
using http::TokenRefusal;
using http::TokenVerdict;

inline constexpr std::chrono::milliseconds patience = 30s;
inline constexpr const char* endpoint_path = "/mcp";
inline constexpr const char* document_path_of_endpoint = "/.well-known/oauth-protected-resource/mcp";
inline constexpr const char* modern_revision = "2026-07-28";
inline constexpr const char* latest_legacy_revision = "2025-11-25";
// The revision that has sessions but no version header.
inline constexpr const char* first_session_revision = "2025-03-26";
inline constexpr const char* json_media_type = "application/json";
inline constexpr const char* both_media_types = "application/json, text/event-stream";
inline constexpr const char* first_issuer = "https://auth.example";
inline constexpr const char* second_issuer = "https://other-auth.example";
inline constexpr const char* configured_resource = "http://localhost/mcp";

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

// A validator from a table: a token it does not know is invalid. Counts its calls and keeps what it was asked.
class Table final : public http::TokenValidator {
public:
    void set(const std::string& token, TokenVerdict verdict) {
        std::lock_guard lock(mutex_);
        verdicts_.insert_or_assign(token, std::move(verdict));
    }
    void validate(const http::TokenCheck& check, http::TokenReply reply) override {
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

// A validator whose way of answering the test chooses per call: `how` gets the validator itself, the check and the
// reply, and may answer at once, later, from another thread, twice, never, or throw. Counts its calls. The threads it
// started are joined when it goes.
class Programmable final : public http::TokenValidator {
public:
    using How = std::function<void(Programmable&, const http::TokenCheck&, http::TokenReply)>;
    explicit Programmable(How how) : how_(std::move(how)) {}
    ~Programmable() override {
        std::vector<std::thread> threads;
        {
            std::lock_guard lock(mutex_);
            threads = std::move(threads_);
        }
        for (std::thread& thread : threads) {
            if (thread.joinable()) thread.join();
        }
    }
    void validate(const http::TokenCheck& check, http::TokenReply reply) override {
        ++calls;
        how_(*this, check, std::move(reply));
    }
    // Runs `work` on a thread that is joined when the validator goes.
    void later(std::function<void()> work) {
        std::lock_guard lock(mutex_);
        threads_.emplace_back(std::move(work));
    }
    // Keeps a reply to answer it later; returns how many are kept.
    std::size_t hold(http::TokenReply reply) {
        std::lock_guard lock(mutex_);
        held_.push_back(std::move(reply));
        return held_.size();
    }
    std::size_t held_count() {
        std::lock_guard lock(mutex_);
        return held_.size();
    }
    // Answers every kept reply with `verdict`, on the calling thread.
    void release(const TokenVerdict& verdict) {
        std::vector<http::TokenReply> replies;
        {
            std::lock_guard lock(mutex_);
            replies = std::move(held_);
            held_.clear();
        }
        for (auto& reply : replies) reply(verdict);
    }

    std::atomic<int> calls{0};

private:
    const How how_;
    std::mutex mutex_;
    std::vector<std::thread> threads_;
    std::vector<http::TokenReply> held_;
};

// What a validated token says: a token for the configured resource, valid for `lifetime`.
inline Authorization granted(const FakeClock& clock, std::string subject, std::vector<std::string> scopes,
                             std::chrono::seconds lifetime = 3600s, std::string issuer = first_issuer,
                             std::vector<std::string> audiences = {configured_resource},
                             std::string client = "client-1") {
    Authorization authorization;
    authorization.issuer = std::move(issuer);
    authorization.subject = std::move(subject);
    authorization.client_id = std::move(client);
    authorization.scopes = std::move(scopes);
    authorization.audiences = std::move(audiences);
    authorization.expires = clock.now() + lifetime;
    return authorization;
}

// The same for a validator that is built before the server and so before its clock: valid for an hour from the real
// time now, which is where a harness's clock starts.
inline Authorization granted_now(std::string subject, std::vector<std::string> scopes) {
    Authorization authorization;
    authorization.issuer = first_issuer;
    authorization.subject = std::move(subject);
    authorization.client_id = "client-1";
    authorization.scopes = std::move(scopes);
    authorization.audiences = {configured_resource};
    authorization.expires = SystemClock::now() + 3600s;
    return authorization;
}

// ---- The server under test -------------------------------------------------------------------------------------------

// How often each item ran, and what the one that tells who asked saw.
struct Items {
    std::atomic<int> whoami{0};
    std::atomic<int> open_tool{0};
    std::atomic<int> guarded{0};
    std::atomic<int> audited{0};
    std::atomic<int> secret{0};
    std::atomic<int> open_resource{0};
    std::atomic<int> document{0};
    std::atomic<int> plan{0};
    std::atomic<int> completed{0};
    std::atomic<int> confirm{0};
    std::atomic<int> hidden_runs{0};
    // The tool that is switched off until a test switches it on (it needs hidden:use).
    std::optional<ToolHandle> hidden;

    int total() const {
        return whoami + open_tool + guarded + audited + secret + open_resource + document + plan + completed + confirm +
               hidden_runs;
    }
};

inline json confirm_schema() {
    return json::parse(R"({"type":"object","properties":{"ok":{"type":"boolean"}},"required":["ok"]})");
}

// Tools: whoami (who asked, as JSON), open_tool (no scope), guarded (files:write), audited (audit:read), confirm (asks
// before it acts; no scope). Resources: test://secret (secret:read), test://open. Template doc://{name} (docs:read),
// prompt plan (plans:read), both with a completion. Everything counts its runs in `items`.
inline void register_items(Server& server, Items& items) {
    server.tool("whoami", "Tells who asked", [&items](Context& context) {
        ++items.whoami;
        json seen = json::object();
        if (const Authorization* who = context.authorization()) {
            seen["issuer"] = who->issuer;
            seen["subject"] = who->subject ? json(*who->subject) : json(nullptr);
            seen["client"] = who->client_id ? json(*who->client_id) : json(nullptr);
            seen["scopes"] = who->scopes;
            seen["audiences"] = who->audiences;
        } else {
            seen["anonymous"] = true;
        }
        return seen.dump();
    });
    server.tool("open_tool", "Needs no scope of its own", [&items] {
        ++items.open_tool;
        return std::string("open");
    });
    server.tool("guarded", "Needs files:write", [&items] {
        ++items.guarded;
        return std::string("written");
    }).scopes({"files:write"});
    server.tool("audited", "Needs audit:read", [&items] {
        ++items.audited;
        return std::string("audited");
    }).scopes({"audit:read"});
    server.tool("confirm", "Asks before it acts",
                [&items](Context& context) -> std::variant<std::string, InputRequired> {
                    ++items.confirm;
                    if (context.input_responses().find("confirm") == context.input_responses().end()) {
                        InputRequired ask;
                        ask.requests.emplace("confirm", ElicitationForm{"Go on?", confirm_schema()});
                        return ask;
                    }
                    return std::string("confirmed");
                });
    items.hidden = server.tool("hidden", "Switched off until a test says otherwise", [&items] {
                          ++items.hidden_runs;
                          return std::string("hidden");
                      })
                       .scopes({"hidden:use"})
                       .enable(false);
    server.resource("test://secret", "Needs secret:read", [&items] {
        ++items.secret;
        return std::string("s");
    }).scopes({"secret:read"});
    server.resource("test://open", "Anyone may read", [&items] {
        ++items.open_resource;
        return std::string("o");
    });
    server.resource_template<DocumentArgs>("doc://{name}", "Needs docs:read", [&items](const DocumentArgs& args) {
        ++items.document;
        return "doc " + args.name;
    })
        .scopes({"docs:read"})
        .complete("name", [&items](std::string_view) {
            ++items.completed;
            return std::vector<std::string>{"alpha"};
        });
    server.prompt<TopicArgs>("plan", "Needs plans:read", [&items](const TopicArgs& args) {
        ++items.plan;
        return "Plan " + args.topic;
    })
        .scopes({"plans:read"})
        .complete("topic", [&items](std::string_view) {
            ++items.completed;
            return std::vector<std::string>{"beta"};
        });
}

inline http::ResourceServerOptions resource_options(const std::shared_ptr<http::TokenValidator>& validator,
                                                    FakeClock& clock, std::string resource = configured_resource) {
    http::ResourceServerOptions options;
    options.resource = std::move(resource);
    options.authorization_servers = {first_issuer};
    options.required_scopes = {"mcp:use"};
    options.scopes_supported = std::vector<std::string>{"mcp:use", "files:read", "files:write"};
    options.resource_name = "Test resource";
    options.validator = validator;
    options.clock = clock.function();
    return options;
}

inline HttpServerOptions quick_options() {
    HttpServerOptions options;
    options.disconnect_poll_interval = 20ms;
    options.endpoint.stream_after = 1h;
    options.keep_alive_interval = 1h;
    options.worker_threads = 4;
    return options;
}

inline ServerOptions server_options() {
    ServerOptions options{.name = "scenario-server", .version = "0.0.1"};
    options.request_state_keys = {std::string(32, 'k')};
    options.scope_implications =
        ScopeImplications(ScopeImplications::Declared{{"admin", {"mcp:use", "files:write"}}});
    return options;
}

// An HttpServer on a loopback port with authorization, over the items above and a table validator.
struct Harness {
    explicit Harness(std::function<void(HttpServerOptions&)> tweak = {},
                     std::function<void(ServerOptions&)> tweak_server = {})
        : server(make_server_options(std::move(tweak_server))) {
        register_items(server, items);
        HttpServerOptions options = quick_options();
        options.authorization = resource_options(table, clock);
        if (tweak) tweak(options);
        http = std::make_unique<HttpServer>(server, std::move(options));
        auto started = http->start();
        if (!started) throw std::runtime_error("start failed: " + started.error);
        port = *started.value;
    }
    ~Harness() {
        if (http) http->stop();
    }
    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    // A token the table knows.
    std::string token(const std::string& name, TokenVerdict verdict) {
        table->set(name, std::move(verdict));
        return name;
    }

    Items items;
    FakeClock clock;
    std::shared_ptr<Table> table = std::make_shared<Table>();
    Server server;
    std::unique_ptr<HttpServer> http;
    std::uint16_t port = 0;

private:
    static ServerOptions make_server_options(std::function<void(ServerOptions&)> tweak) {
        ServerOptions options = server_options();
        if (tweak) tweak(options);
        return options;
    }
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
    json message() const { return json::parse(body); }

    // Everything the peer can read of the answer, as one text: its fields and its body.
    std::string everything() const {
        std::string text = std::to_string(status) + "\n";
        for (const auto& [name, value] : headers) text += name + ": " + value + "\n";
        return text + body;
    }
};

inline std::unique_ptr<httplib::Client> make_client(std::uint16_t port) {
    auto client = std::make_unique<httplib::Client>("127.0.0.1", port);
    client->set_keep_alive(false);
    client->set_connection_timeout(30, 0);
    client->set_read_timeout(60, 0);
    client->set_write_timeout(60, 0);
    return client;
}

inline Reply send(std::uint16_t port, const std::string& method, httplib::Headers headers, std::string body = {},
                  const std::string& target = endpoint_path) {
    auto client = make_client(port);
    httplib::Request request;
    request.method = method;
    request.path = target;
    request.headers = std::move(headers);
    request.body = std::move(body);
    Reply reply;
    if (auto result = client->send(request)) {
        reply.ok = true;
        reply.status = result->status;
        reply.headers = result->headers;
        reply.body = result->body;
    }
    return reply;
}

// One request that runs on a thread of its own and is read as it arrives.
class Call {
public:
    Call(std::uint16_t port, std::string method, httplib::Headers headers, std::string body = {},
         const std::string& target = endpoint_path)
        : client_(make_client(port)) {
        request_.method = std::move(method);
        request_.path = target;
        request_.headers = std::move(headers);
        request_.body = std::move(body);
        request_.response_handler = [this](const httplib::Response& response) {
            {
                std::lock_guard lock(mutex_);
                reply_.status = response.status;
                reply_.headers = response.headers;
                have_headers_ = true;
            }
            changed_.notify_all();
            return true;
        };
        request_.content_receiver = [this](const char* data, std::size_t length, std::size_t, std::size_t) {
            {
                std::lock_guard lock(mutex_);
                reply_.body.append(data, length);
            }
            changed_.notify_all();
            return true;
        };
        thread_ = std::thread([this] {
            const auto result = client_->send(request_);
            {
                std::lock_guard lock(mutex_);
                reply_.ok = static_cast<bool>(result);
                if (result && !have_headers_) {
                    reply_.status = result->status;
                    reply_.headers = result->headers;
                    reply_.body = result->body;
                }
                finished_ = true;
            }
            changed_.notify_all();
        });
    }
    Call(const Call&) = delete;
    Call& operator=(const Call&) = delete;
    ~Call() {
        client_->stop();
        if (thread_.joinable()) thread_.join();
    }

    bool wait_for_headers() {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, patience, [&] { return have_headers_ || finished_; });
        return have_headers_;
    }
    // Waits until the predicate holds for the body so far, or the request ended, or the time is out.
    bool wait_until(const std::function<bool(const std::string&)>& predicate,
                    std::chrono::milliseconds timeout = patience) {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, timeout, [&] { return predicate(reply_.body) || finished_; });
        return predicate(reply_.body);
    }
    Reply finish(std::chrono::milliseconds timeout = patience) {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, timeout, [&] { return finished_; });
        return reply_;
    }
    bool finished() {
        std::lock_guard lock(mutex_);
        return finished_;
    }
    Reply now() {
        std::lock_guard lock(mutex_);
        return reply_;
    }
    // Closes the connection from the client's side.
    void disconnect() { client_->stop(); }

private:
    std::unique_ptr<httplib::Client> client_;
    httplib::Request request_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable changed_;
    Reply reply_;
    bool have_headers_ = false;
    bool finished_ = false;
};

// ---- Requests --------------------------------------------------------------------------------------------------------

inline std::string rpc(const json& id, const std::string& method, json params = json::object()) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}}.dump();
}

// The members every request of the 2026-07-28 revision carries; `capabilities` is what the client says it can take.
inline json modern_params(json params = json::object(), json capabilities = json::object()) {
    params["_meta"] = json{{"io.modelcontextprotocol/protocolVersion", modern_revision},
                           {"io.modelcontextprotocol/clientInfo", {{"name", "test-client"}, {"version", "1.0.0"}}},
                           {"io.modelcontextprotocol/clientCapabilities", std::move(capabilities)}};
    return params;
}

inline httplib::Headers modern_headers(const std::string& method, const std::optional<std::string>& token,
                                       std::optional<std::string> name = std::nullopt) {
    httplib::Headers headers{{"Content-Type", json_media_type},
                             {"Accept", both_media_types},
                             {"MCP-Protocol-Version", modern_revision},
                             {"Mcp-Method", method}};
    if (name) headers.emplace("Mcp-Name", *name);
    if (token) headers.emplace("Authorization", "Bearer " + *token);
    return headers;
}

// A request of the 2026-07-28 revision; `name` is the Mcp-Name field the revision asks for on some methods.
inline Reply modern_request(std::uint16_t port, const std::optional<std::string>& token, const std::string& method,
                            json params = json::object(), std::optional<std::string> name = std::nullopt,
                            json capabilities = json::object()) {
    return send(port, "POST", modern_headers(method, token, std::move(name)),
                rpc(1, method, modern_params(std::move(params), std::move(capabilities))));
}

inline Reply call_tool(std::uint16_t port, const std::optional<std::string>& token, const std::string& tool) {
    return modern_request(port, token, "tools/call", json{{"name", tool}, {"arguments", json::object()}}, tool);
}

inline Reply list_tools(std::uint16_t port, const std::optional<std::string>& token) {
    return modern_request(port, token, "tools/list");
}

inline httplib::Headers legacy_headers(const std::string& session, const std::string& token,
                                       const std::string& revision = latest_legacy_revision) {
    httplib::Headers headers{{"Content-Type", json_media_type},
                             {"Accept", both_media_types},
                             {"Authorization", "Bearer " + token}};
    if (revision != first_session_revision) headers.emplace("MCP-Protocol-Version", revision);
    if (!session.empty()) headers.emplace("Mcp-Session-Id", session);
    return headers;
}

// A session of a revision that keeps one, opened with `token`; its id.
inline std::string open_session(std::uint16_t port, const std::string& token,
                                const std::string& revision = latest_legacy_revision) {
    const json params{{"protocolVersion", revision},
                      {"capabilities", json::object()},
                      {"clientInfo", {{"name", "test-client"}, {"version", "1.0.0"}}}};
    const Reply opened = send(port, "POST", legacy_headers({}, token, revision), rpc(1, "initialize", params));
    if (opened.status != 200) throw std::runtime_error("initialize answered " + std::to_string(opened.status));
    const std::string id = opened.header("Mcp-Session-Id");
    const Reply ready = send(port, "POST", legacy_headers(id, token, revision),
                             json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}.dump());
    if (ready.status != 202) throw std::runtime_error("initialized answered " + std::to_string(ready.status));
    return id;
}

inline Reply ping(std::uint16_t port, const std::string& session, const std::string& token,
                  const std::string& revision = latest_legacy_revision) {
    return send(port, "POST", legacy_headers(session, token, revision), rpc(2, "ping"));
}

// A request inside a session of a revision that keeps one.
inline Reply session_request(std::uint16_t port, const std::string& session, const std::string& token,
                             const std::string& method, json params, const std::string& revision = latest_legacy_revision) {
    return send(port, "POST", legacy_headers(session, token, revision), rpc(3, method, std::move(params)));
}

// ---- Reading answers ---------------------------------------------------------------------------------------------------

// The Bearer challenge of a reply.
inline http::BearerChallenge challenge_of(const Reply& reply) {
    std::vector<std::string> values;
    for (auto [at, end] = reply.headers.equal_range("WWW-Authenticate"); at != end; ++at) values.push_back(at->second);
    auto parsed = http::parse_challenges(std::span<const std::string>(values));
    const auto* challenges = std::get_if<std::vector<http::Challenge>>(&parsed);
    if (challenges == nullptr) throw std::runtime_error("the challenge cannot be read");
    auto bearer = http::find_bearer_challenge(*challenges);
    const auto* found = std::get_if<http::BearerChallenge>(&bearer);
    if (found == nullptr) throw std::runtime_error("no Bearer challenge");
    return *found;
}

inline std::string error_of(const Reply& reply) { return json::parse(reply.body).at("error").get<std::string>(); }

inline std::string text_of(const Reply& reply) {
    return json::parse(reply.body).at("result").at("content").at(0).at("text").get<std::string>();
}

// The scopes a challenge lists, in order.
inline std::vector<std::string> scopes_of(const Reply& reply) { return challenge_of(reply).scopes(); }

}  // namespace neomcp::scenario

