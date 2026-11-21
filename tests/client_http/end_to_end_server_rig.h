// Shared by the tests of the library's OAuth client against the library's own protected server (the ServerEndToEnd
// tests): the server, a Streamable HTTP endpoint with authorization on its own listener, and the scripted authorization
// servers of oauth_rig.h behind it.
//
// The server's resource identifier names the port its listener bound, so the listener is bound first, with forwarding
// handlers, and the endpoint is made once the port is known. The forwarders also record what reaches the server, which
// lets a test say in what order the client asked for things: the protected resource metadata by the URL the challenge
// names, then the token's first use.
//
// The validator the server trusts is a table of what the scripted authorization servers issued: their records say
// which resource each token names, for which client, and until when. The tests of the introspection and the JWT
// validators put those validators in its place.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/client.h"
#include "neomcp/http/endpoint.h"
#include "neomcp/http/listener.h"
#include "neomcp/http/policy.h"
#include "neomcp/http/resource_server.h"
#include "neomcp/server.h"
#include "oauth_rig.h"

namespace server_rig {

using namespace std::chrono_literals;
using oauth_rig::AuthServer;
using oauth_rig::Ledger;
using oauth_rig::World;
using oauth_rig::json;

inline constexpr std::string_view endpoint_path = "/mcp";
inline constexpr std::string_view document_path = "/.well-known/oauth-protected-resource/mcp";

// The scopes the server's tools declare, and the tools' names.
inline constexpr std::string_view write_scope = "files:write";
inline constexpr std::string_view ungranted_scope = "never:granted";
inline constexpr std::string_view required_scope = "mcp:use";

// ---- What reached the server

struct Seen {
    std::string method;
    std::string path;
    std::string rpc_method;                  // the JSON-RPC method of a body that has one
    std::optional<std::string> authorization;  // the whole value of the Authorization field
    int status = 0;                          // what the server answered with; 0 until it did
};

class RequestLog {
public:
    // Returns the request's place, for its answer.
    std::size_t add(Seen seen) {
        std::lock_guard lock(mutex_);
        seen_.push_back(std::move(seen));
        return seen_.size() - 1;
    }
    void answered(std::size_t place, int status) {
        std::lock_guard lock(mutex_);
        seen_[place].status = status;
    }
    std::vector<Seen> all() const {
        std::lock_guard lock(mutex_);
        return seen_;
    }
    // The requests to one path with one method.
    std::vector<Seen> to(std::string_view method, std::string_view path) const {
        std::vector<Seen> out;
        for (Seen& one : all()) {
            if (one.method == method && one.path == path) out.push_back(std::move(one));
        }
        return out;
    }

private:
    mutable std::mutex mutex_;
    std::vector<Seen> seen_;
};

// ---- The validator of the scripted authorization servers' tokens

// A token is good while the ledger holds it; what it says comes from the grant of the authorization server that issued
// it. A token of no server, or one the ledger dropped (it expired), is invalid.
class IssuedTable final : public neomcp::http::TokenValidator {
public:
    explicit IssuedTable(Ledger& ledger) : ledger_(ledger) {}

    void add(const AuthServer& server) {
        std::lock_guard lock(mutex_);
        servers_.push_back(&server);
    }

    void validate(const neomcp::http::TokenCheck& check, neomcp::http::TokenReply reply) override {
        const std::string token(check.token);
        {
            std::lock_guard lock(mutex_);
            ++calls_;
            resources_.emplace_back(check.resource);
        }
        reply(verdict_for(token));
    }

    int calls() const {
        std::lock_guard lock(mutex_);
        return calls_;
    }
    std::vector<std::string> resources() const {
        std::lock_guard lock(mutex_);
        return resources_;
    }

private:
    neomcp::http::TokenVerdict verdict_for(const std::string& token) const {
        const auto scopes = ledger_.scopes_of(token);
        if (!scopes) return neomcp::http::TokenRefusal::invalid;
        std::vector<const AuthServer*> servers;
        {
            std::lock_guard lock(mutex_);
            servers = servers_;
        }
        for (const AuthServer* server : servers) {
            const auto grant = server->grant_of(token);
            if (!grant) continue;
            neomcp::Authorization granted;
            granted.issuer = server->issuer();
            granted.subject = grant->subject;
            granted.client_id = grant->client_id;
            granted.scopes = *scopes;
            if (!grant->audience.empty()) granted.audiences.push_back(grant->audience);
            granted.expires = grant->expires;
            return granted;
        }
        return neomcp::http::TokenRefusal::invalid;
    }

    Ledger& ledger_;
    mutable std::mutex mutex_;
    std::vector<const AuthServer*> servers_;
    int calls_ = 0;
    std::vector<std::string> resources_;
};

// ---- The server

// How often each tool ran.
struct Items {
    std::atomic<int> open{0};
    std::atomic<int> write{0};
    std::atomic<int> ungranted{0};
    std::atomic<int> whoami{0};
    std::atomic<int> hidden{0};
    // The tool that is switched off until a test switches it on: a change of the list the client can hear.
    std::optional<neomcp::ToolHandle> switched;
};

inline void register_items(neomcp::Server& server, Items& items) {
    server.tool("open", "Needs no scope of its own", [&items] {
        ++items.open;
        return std::string("open");
    });
    server.tool("write", "Needs the write scope", [&items] {
        ++items.write;
        return std::string("written");
    }).scopes({std::string(write_scope)});
    server.tool("ungranted", "Needs a scope the authorization servers never grant", [&items] {
        ++items.ungranted;
        return std::string("never");
    }).scopes({std::string(ungranted_scope)});
    server.tool("whoami", "Tells who asked", [&items](neomcp::Context& context) {
        ++items.whoami;
        json seen = json::object();
        if (const neomcp::Authorization* who = context.authorization()) {
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
    items.switched = server.tool("hidden", "Switched off until a test switches it on", [&items] {
                               ++items.hidden;
                               return std::string("hidden");
                           })
                         .enable(false);
}

struct ServerWorld;

struct ServerSetup {
    // The scopes every request needs (the 401's `scope`), and the metadata's `scopes_supported` (none: the server's
    // default, which is the required scopes).
    std::vector<std::string> required_scopes{std::string(required_scope)};
    std::optional<std::vector<std::string>> scopes_supported;
    // The validator the server trusts, made once the authorization servers exist; none: the table of their tokens.
    std::function<std::shared_ptr<neomcp::http::TokenValidator>(ServerWorld&)> validator;
    // Last say on the server's authorization options.
    std::function<void(neomcp::http::ResourceServerOptions&)> authorization;
};

// The server, two scripted authorization servers (the first is the one the server names; the second is for a switch),
// the person, the client's token store and everything of World, which supplies the client's side. World's scripted
// resource is not used here.
struct ServerWorld : World {
    // Hands each request on to the handler the endpoint supplies once it exists, and records it.
    struct Forward {
        std::mutex mutex;
        neomcp::http::Handler main;
        neomcp::http::Handler document;
        RequestLog log;
        void call(bool for_document, neomcp::http::Request request, neomcp::http::Responder responder) {
            Seen seen;
            seen.method = request.method;
            seen.path = request.path;
            if (const auto given = request.headers.get("Authorization")) seen.authorization = std::string(*given);
            if (!request.body.empty()) {
                const json message = json::parse(request.body, nullptr, false);
                const auto* members = message.get_ptr<const json::object_t*>();
                if (members != nullptr) {
                    const auto found = members->find("method");
                    if (found != members->end() && found->second.is_string()) {
                        seen.rpc_method = found->second.get<std::string>();
                    }
                }
            }
            const std::size_t place = log.add(std::move(seen));
            neomcp::http::Handler handler;
            {
                std::lock_guard lock(mutex);
                handler = for_document ? document : main;
            }
            if (!handler) {
                responder.complete(neomcp::http::make_response(503));
                return;
            }
            // The endpoint answers through a responder that notes the status on its way, and is told of the peer's
            // disconnect as the original is.
            neomcp::http::Responder noting([this, place, original = responder](neomcp::http::Response&& answer) {
                log.answered(place, answer.status);
                return original.complete(std::move(answer));
            });
            responder.on_disconnect([watch = noting.watch()] { watch.report_disconnect(); });
            handler(std::move(request), std::move(noting));
        }
    };

    explicit ServerWorld(ServerSetup setup = {})
        : server(neomcp::ServerOptions{.name = "end-to-end-server", .version = "1.0.0"}) {
        register_items(server, items);
        table = std::make_shared<IssuedTable>(ledger);
        table->add(as);
        table->add(second);

        neomcp::http::ListenerOptions listening;
        listening.path = std::string(endpoint_path);
        listening.disconnect_poll_interval = 20ms;
        listening.keep_alive_interval = 1h;
        listening.worker_threads = 4;
        listening.max_worker_threads = 32;
        listening.routes.push_back(neomcp::http::Route{
            std::string(document_path), [forward = forward](neomcp::http::Request request, neomcp::http::Responder responder) {
                forward->call(true, std::move(request), std::move(responder));
            }});
        listener = std::make_unique<neomcp::http::Listener>(
            std::move(listening), [forward = forward](neomcp::http::Request request, neomcp::http::Responder responder) {
                forward->call(false, std::move(request), std::move(responder));
            });
        auto bound = listener->start();
        if (!bound) throw std::runtime_error("listen failed: " + bound.error);
        port = *bound.value;
        resource_url = "http://127.0.0.1:" + std::to_string(port) + std::string(endpoint_path);

        neomcp::http::PolicyOptions policy_options;
        policy_options.bound_port = port;
        auto policy = neomcp::http::HostPolicy::create(policy_options);
        if (!policy) throw std::runtime_error(policy.error);

        neomcp::http::ResourceServerOptions authorization;
        authorization.resource = resource_url;
        authorization.authorization_servers = {as.issuer()};
        authorization.required_scopes = std::move(setup.required_scopes);
        authorization.scopes_supported = std::move(setup.scopes_supported);
        authorization.resource_name = "End to end server";
        authorization.validator = setup.validator ? setup.validator(*this)
                                                  : std::static_pointer_cast<neomcp::http::TokenValidator>(table);
        if (setup.authorization) setup.authorization(authorization);
        neomcp::http::EndpointOptions served;
        served.stream_after = 1h;
        served.authorization = std::move(authorization);
        auto made = neomcp::http::Endpoint::create(server, std::move(*policy.value), std::move(served));
        if (!made) throw std::runtime_error(made.error);
        endpoint = std::move(*made.value);
        std::lock_guard lock(forward->mutex);
        forward->main = endpoint->handler();
        forward->document = endpoint->metadata_handler();
    }

    ~ServerWorld() {
        if (endpoint) endpoint->stop();
        listener.reset();
        endpoint.reset();
    }
    ServerWorld(const ServerWorld&) = delete;
    ServerWorld& operator=(const ServerWorld&) = delete;

    // A client of the server, signing in with `given`.
    neomcp::Result<neomcp::Client> connect(std::shared_ptr<neomcp::HttpAuthorizer> given,
                                           const std::function<void(neomcp::ClientOptions&)>& tweak = {}) {
        auto described = http_rig::endpoint(resource_url);
        described.authorizer = std::move(given);
        neomcp::ClientOptions settings = client_options();
        if (tweak) tweak(settings);
        return neomcp::Client::open(std::move(settings), std::move(described));
    }
    neomcp::Result<neomcp::Client> connect() { return connect(authorizer()); }

    RequestLog& log() { return forward->log; }

    // The server names these authorization servers from now on.
    void name_authorization_servers(std::vector<std::string> issuers) {
        const auto updated = endpoint->update_authorization_servers(std::move(issuers));
        EXPECT_TRUE(updated) << updated.error;
    }

    AuthServer second{ledger, "two"};
    Items items;
    std::shared_ptr<Forward> forward = std::make_shared<Forward>();
    neomcp::Server server;
    std::shared_ptr<IssuedTable> table;
    std::unique_ptr<neomcp::http::Listener> listener;
    std::unique_ptr<neomcp::http::Endpoint> endpoint;
    std::uint16_t port = 0;
    std::string resource_url;
};

// ---- Reading what came back

inline std::string text_of(const neomcp::ToolResult& result) {
    for (const auto& block : result.content) {
        if (const auto* text = std::get_if<neomcp::TextContent>(&block)) return text->text;
    }
    return {};
}

// The scope of the authorization URL of a prompt; none when the URL asks for none.
inline std::optional<std::string> asked_scope(const neomcp::AuthorizationPromptInfo& prompt) {
    return oauth_rig::field_of(oauth_rig::query_of(prompt.authorization_url), "scope");
}

// The `resource` of the authorization URL of a prompt.
inline std::optional<std::string> asked_resource(const neomcp::AuthorizationPromptInfo& prompt) {
    return oauth_rig::field_of(oauth_rig::query_of(prompt.authorization_url), "resource");
}

// The revisions the client enables: one, or (for "all") every built-in one.
inline constexpr std::string_view every_revision = "all";
inline void use_revision(World& world, const std::string& revision) {
    world.revisions.clear();
    if (revision != every_revision) world.revisions.push_back(revision);
}

// The name the tests of one revision are given.
inline std::string revision_name(const std::string& revision) {
    std::string name = revision;
    for (char& c : name) {
        if (c == '-') c = '_';
    }
    return "Revision_" + name;
}

// Whether `holds` is true within `bound`, looked at every few milliseconds.
inline bool wait_until(const std::function<bool()>& holds, std::chrono::milliseconds bound = 20s) {
    const auto until = std::chrono::steady_clock::now() + bound;
    while (!holds()) {
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

// The statuses the server answered the requests that carried `token`, in order.
inline std::vector<int> statuses_for(ServerWorld& world, const std::string& token) {
    std::vector<int> out;
    for (const Seen& one : world.log().all()) {
        if (one.authorization == "Bearer " + token) out.push_back(one.status);
    }
    return out;
}

// Takes the changes of a subscription on a thread of its own, so that a test never waits on next() itself.
class ChangeCollector {
public:
    explicit ChangeCollector(neomcp::Subscription taken)
        : subscription_(std::make_unique<neomcp::Subscription>(std::move(taken))),
          reader_([this] { read(); }) {}
    ~ChangeCollector() {
        (void)subscription_->close();
        if (reader_.joinable()) reader_.join();
    }
    ChangeCollector(const ChangeCollector&) = delete;
    ChangeCollector& operator=(const ChangeCollector&) = delete;

    neomcp::Subscription& subscription() { return *subscription_; }

    std::size_t count() const {
        std::lock_guard lock(mutex_);
        return changes_.size();
    }
    // Waits until `total` changes arrived altogether.
    bool wait_for(std::size_t total, std::chrono::milliseconds bound = 20s) const {
        return wait_until([&] { return count() >= total; }, bound);
    }
    // The error that ended the subscription, once it ended.
    std::optional<neomcp::ClientError> ended() const {
        std::lock_guard lock(mutex_);
        return ended_;
    }

private:
    void read() {
        for (;;) {
            auto next = subscription_->next();
            std::lock_guard lock(mutex_);
            if (!next) {
                ended_ = next.error();
                return;
            }
            changes_.push_back(*next);
        }
    }

    std::unique_ptr<neomcp::Subscription> subscription_;
    mutable std::mutex mutex_;
    std::vector<neomcp::Change> changes_;
    std::optional<neomcp::ClientError> ended_;
    std::thread reader_;
};

// No token any authorization server issued appears in the targets of what the client asked a server for, in the
// client's errors or in its diagnostics, or in the server's answers a handler wrote (`texts`).
inline void expect_tokens_kept(ServerWorld& world, const std::vector<neomcp::ClientError>& errors = {},
                               const std::vector<std::string>& texts = {}) {
    std::vector<std::string> secrets = world.secrets({&world.second});
    oauth_rig::expect_quiet(world, errors, secrets);
    oauth_rig::expect_no_secret_in_targets(world.as.server(), secrets);
    oauth_rig::expect_no_secret_in_targets(world.second.server(), secrets);
    for (const Seen& one : world.log().all()) {
        oauth_rig::expect_none_of(one.path, secrets, "a request path of the server");
    }
    for (const std::string& text : texts) oauth_rig::expect_none_of(text, secrets, "a text of a handler");
}

// ---- What the tests of every validator check

using oauth_rig::field_of;

inline std::string join_scopes(const std::vector<std::string>& scopes) {
    std::string out;
    for (const std::string& one : scopes) {
        if (!out.empty()) out.push_back(' ');
        out += one;
    }
    return out;
}

inline long long epoch_seconds(std::chrono::system_clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
}

// The status of the newest request that carried a token.
inline int last_authorized_status(ServerWorld& world) {
    const std::vector<Seen> all = world.log().all();
    for (auto one = all.rbegin(); one != all.rend(); ++one) {
        if (one->authorization) return one->status;
    }
    return 0;
}

// What the client does against any validator: signs in at the first refusal, the call passes, a tool that declares a
// scope makes it sign in once more for the union, and what a handler sees is what the token said.
inline void expect_first_contact_and_step_up(ServerWorld& world, const std::string& revision) {
    use_revision(world, revision);
    auto client = world.connect();
    ASSERT_TRUE(client) << client.error().message;
    const auto opened = client->call_tool("open");
    ASSERT_TRUE(opened) << opened.error().message;
    EXPECT_EQ(text_of(*opened), "open");
    ASSERT_EQ(world.person.prompts().size(), 1u);
    EXPECT_EQ(asked_scope(world.person.prompts().front()), std::string(required_scope));
    EXPECT_EQ(world.as.requests_to("/register").size(), 1u);

    const auto written = client->call_tool("write");
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(text_of(*written), "written");
    ASSERT_EQ(world.person.prompts().size(), 2u);
    EXPECT_EQ(asked_scope(world.person.prompts()[1]), "mcp:use files:write");
    ASSERT_EQ(world.as.access_tokens().size(), 2u);

    const auto who = client->call_tool("whoami");
    ASSERT_TRUE(who) << who.error().message;
    const json seen = json::parse(text_of(*who));
    EXPECT_EQ(seen.at("issuer"), world.as.issuer());
    EXPECT_EQ(seen.at("subject"), std::string(AuthServer::subject_name));
    EXPECT_EQ(seen.at("client"), world.as.client_id());
    EXPECT_EQ(seen.at("scopes"), json::array({"mcp:use", "files:write"}));
    EXPECT_EQ(seen.at("audiences"), json::array({world.resource_url}));
    // The server went to the table of issued tokens for none of them.
    EXPECT_EQ(world.table->calls(), 0);
    EXPECT_EQ(last_authorized_status(world), 200);
    expect_tokens_kept(world, {}, {text_of(*who)});
    EXPECT_TRUE(client->close());
}

}  // namespace server_rig
