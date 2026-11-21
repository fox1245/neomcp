// Tests of the scopes a server's items declare, checked against the authorization a transport gives with each input
// unit: every kind of item, the implications the server declares, a batch, what a handler sees, and a retried call
// under another principal. The session is fed units directly, as a transport that authorizes would feed it.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <asio/awaitable.hpp>
#include <gtest/gtest.h>

#include "neomcp/authorization.h"
#include "neomcp/server.h"
#include "neomcp/testing/peer.h"

namespace sat {

struct DocParams {
    std::string name;
};

struct TopicArgs {
    std::string topic;
};

}  // namespace sat

NEOMCP_DESCRIBE(sat::DocParams, name, "the document")
NEOMCP_DESCRIBE(sat::TopicArgs, topic, "what to plan")

namespace {

using namespace std::chrono_literals;
using neomcp::json;
using neomcp::testing::Peer;
namespace profile = neomcp::profile;
using Scopes = std::vector<std::string>;
using Granted = std::shared_ptr<const neomcp::Authorization>;

constexpr std::chrono::milliseconds patience = 30s;
constexpr int invalid_request_code = -32600;

profile::ProfilePtr builtin(const std::string& revision) {
    auto found = profile::builtin(revision);
    if (!found) throw std::logic_error("no built-in profile " + revision);
    return found;
}

Granted grant(Scopes scopes, std::string subject = "alice", std::string client = "client-1") {
    auto made = std::make_shared<neomcp::Authorization>();
    made->issuer = "https://as.example";
    made->subject = std::move(subject);
    made->client_id = std::move(client);
    made->scopes = std::move(scopes);
    made->audiences = {"https://mcp.example/mcp"};
    return made;
}

// What the handlers did.
struct Counts {
    std::atomic<int> write{0};
    std::atomic<int> read_files{0};
    std::atomic<int> admin{0};
    std::atomic<int> secret{0};
    std::atomic<int> doc{0};
    std::atomic<int> plan{0};
    std::atomic<int> completed{0};
    std::atomic<int> confirm{0};
};

// What a handler saw of its authorization.
struct Seen {
    std::mutex mutex;
    bool present = false;
    std::string issuer;
    std::optional<std::string> subject;
    std::optional<std::string> client_id;
    Scopes scopes;

    void take(const neomcp::Context& ctx) {
        const std::lock_guard lock(mutex);
        const neomcp::Authorization* who = ctx.authorization();
        present = who != nullptr;
        if (who == nullptr) return;
        issuer = who->issuer;
        subject = who->subject;
        client_id = who->client_id;
        scopes = who->scopes;
    }
};

neomcp::ServerOptions options() {
    neomcp::ServerOptions made{.name = "scoped", .version = "1.0.0"};
    made.request_state_keys = {std::string(32, 'k')};
    // Two steps from admin to files:read: a closure of one step is not enough.
    made.scope_implications = neomcp::ScopeImplications({{"admin", {"files:write"}}, {"files:write", {"files:read"}}});
    return made;
}

json confirm_schema() { return json::parse(R"({"type":"object","properties":{"ok":{"type":"boolean"}},"required":["ok"]})"); }

void register_items(neomcp::Server& server, Counts& counts, Seen& plain, Seen& coroutine) {
    server.tool("open", "Needs nothing", [] { return std::string("open"); });
    server.tool("write", "Needs files:write", [&counts] {
        ++counts.write;
        return std::string("written");
    }).scopes({"files:write"});
    server.tool("read_files", "Needs files:read", [&counts] {
        ++counts.read_files;
        return std::string("read");
    }).scopes({"files:read"});
    server.tool("admin_only", "Needs admin", [&counts] {
        ++counts.admin;
        return std::string("admin");
    }).scopes({"admin"});
    server.tool("who", "Tells who asked", [&plain](neomcp::Context& ctx) {
        plain.take(ctx);
        return std::string("seen");
    });
    server.tool("who_async", "Tells who asked, as a coroutine",
                [&coroutine](neomcp::Context& ctx) -> asio::awaitable<std::string> {
                    coroutine.take(ctx);
                    co_return std::string("seen");
                });
    server.tool("confirm", "Asks before it acts",
                [&counts](neomcp::Context& ctx) -> std::variant<std::string, neomcp::InputRequired> {
                    ++counts.confirm;
                    if (ctx.input_responses().find("confirm") == ctx.input_responses().end()) {
                        neomcp::InputRequired ask;
                        ask.requests.emplace("confirm", neomcp::ElicitationForm{"Go on?", confirm_schema()});
                        return ask;
                    }
                    return std::string("confirmed");
                });
    server.resource("res://secret", "A secret", [&counts] {
        ++counts.secret;
        return std::string("secret");
    }).scopes({"secret:read"});
    server.resource_template<sat::DocParams>("doc://{name}", "A document", [&counts](const sat::DocParams& p) {
        ++counts.doc;
        return "doc " + p.name;
    })
        .scopes({"docs:read"})
        .complete("name", [&counts](std::string_view) {
            ++counts.completed;
            return std::vector<std::string>{"alpha"};
        });
    server.prompt<sat::TopicArgs>("plan", "A plan", [&counts](const sat::TopicArgs& a) {
        ++counts.plan;
        return "Plan " + a.topic;
    })
        .scopes({"plans:read"})
        .complete("topic", [&counts](std::string_view) {
            ++counts.completed;
            return std::vector<std::string>{"beta"};
        });
}

// Feeds units to one session of the server, as a transport that authorizes does, and waits for each unit's last
// output.
class Rig {
public:
    explicit Rig(neomcp::Server& server) {
        auto made = server.connect_tagged(profile::Transport::stdio, [this](neomcp::Output output) { record(std::move(output)); });
        if (auto* error = std::get_if<neomcp::CreateError>(&made)) throw std::runtime_error(error->message);
        session_ = std::get<std::unique_ptr<neomcp::Session>>(std::move(made));
    }

    neomcp::Output send(const json& message, const Granted& who) {
        const neomcp::UnitId unit = ++next_;
        session_->receive(unit, neomcp::serialize(message), {}, who);
        std::unique_lock lock(mutex_);
        if (!arrived_.wait_for(lock, patience, [&] { return last_.count(unit) != 0; })) {
            throw std::runtime_error("no last output of a unit within the patience");
        }
        return last_.at(unit);
    }

private:
    void record(neomcp::Output output) {
        if (!output.unit || !output.last) return;
        const std::lock_guard lock(mutex_);
        last_.emplace(*output.unit, std::move(output));
        arrived_.notify_all();
    }

    std::mutex mutex_;
    std::condition_variable arrived_;
    std::map<neomcp::UnitId, neomcp::Output> last_;
    neomcp::UnitId next_ = 0;
    std::unique_ptr<neomcp::Session> session_;  // last: closed before the members its sink uses go
};

json body_of(const neomcp::Output& output) { return json::parse(output.line); }

json call(const profile::Profile& revision, int id, const std::string& tool) {
    return Peer::make_request(revision, id, "tools/call", json{{"name", tool}, {"arguments", json::object()}});
}

// A call whose client can take a form, under a revision whose requests name it.
json call_with_forms(const profile::Profile& revision, int id, const std::string& tool) {
    json params{{"name", tool}, {"arguments", json::object()}};
    params["_meta"] = Peer::request_meta(revision, json{{"elicitation", json{{"form", json::object()}}}});
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"}, {"params", std::move(params)}};
}

// Whether the output refuses the request for its scopes, naming exactly `missing`.
void expect_scope_refusal(const neomcp::Output& output, const Scopes& missing) {
    EXPECT_EQ(output.refusal, neomcp::RefusalReason::insufficient_scope);
    EXPECT_EQ(output.required_scopes, missing);
    const json body = body_of(output);
    ASSERT_TRUE(body.contains("error")) << output.line;
    EXPECT_EQ(body.at("error").at("code"), invalid_request_code);
}

void expect_result(const neomcp::Output& output) {
    EXPECT_EQ(output.refusal, neomcp::RefusalReason::none);
    EXPECT_TRUE(output.required_scopes.empty());
    EXPECT_TRUE(body_of(output).contains("result")) << output.line;
}

struct Fixture : ::testing::Test {
    Fixture() { register_items(server, counts, plain, coroutine); }
    Counts counts;
    Seen plain;
    Seen coroutine;
    neomcp::Server server{options()};
    profile::ProfilePtr modern = builtin("2026-07-28");
};

using ServerAuthorization = Fixture;

TEST_F(ServerAuthorization, WithoutAnAuthorizationNothingIsCheckedAsToday) {
    Rig rig(server);
    expect_result(rig.send(call(*modern, 1, "write"), nullptr));
    expect_result(rig.send(call(*modern, 2, "admin_only"), nullptr));
    expect_result(rig.send(Peer::make_request(*modern, 3, "resources/read", json{{"uri", "res://secret"}}), nullptr));
    expect_result(rig.send(Peer::make_request(*modern, 4, "prompts/get",
                                              json{{"name", "plan"}, {"arguments", json{{"topic", "t"}}}}),
                           nullptr));
    expect_result(rig.send(call(*modern, 5, "who"), nullptr));
    EXPECT_EQ(counts.write, 1);
    EXPECT_EQ(counts.admin, 1);
    EXPECT_EQ(counts.secret, 1);
    EXPECT_EQ(counts.plan, 1);
    EXPECT_FALSE(plain.present);
}

TEST_F(ServerAuthorization, AToolRunsOnlyWithTheScopesItDeclares) {
    Rig rig(server);
    expect_scope_refusal(rig.send(call(*modern, 1, "write"), grant({})), {"files:write"});
    EXPECT_EQ(body_of(rig.send(call(*modern, 2, "write"), grant({"other"}))).at("id"), 2);
    EXPECT_EQ(counts.write, 0);
    expect_result(rig.send(call(*modern, 3, "write"), grant({"files:write"})));
    EXPECT_EQ(counts.write, 1);
    // An item that declares nothing needs nothing; an unknown one gets its usual answer.
    expect_result(rig.send(call(*modern, 4, "open"), grant({})));
    const neomcp::Output unknown = rig.send(call(*modern, 5, "missing"), grant({}));
    EXPECT_EQ(unknown.refusal, neomcp::RefusalReason::none);
    EXPECT_TRUE(unknown.required_scopes.empty());
}

TEST_F(ServerAuthorization, DeclaredImplicationsAreFollowedThroughEveryStepAndTextImpliesNothing) {
    Rig rig(server);
    expect_result(rig.send(call(*modern, 1, "read_files"), grant({"admin"})));        // admin, files:write, files:read
    expect_result(rig.send(call(*modern, 2, "read_files"), grant({"files:write"})));  // one step
    expect_result(rig.send(call(*modern, 3, "write"), grant({"admin"})));
    EXPECT_EQ(counts.read_files, 2);
    expect_scope_refusal(rig.send(call(*modern, 4, "read_files"), grant({"files:*"})), {"files:read"});
    expect_scope_refusal(rig.send(call(*modern, 5, "admin_only"), grant({"files:write"})), {"admin"});
    EXPECT_EQ(counts.read_files, 2);
    EXPECT_EQ(counts.admin, 0);
}

TEST_F(ServerAuthorization, AResourceAndATemplateNeedTheirScopes) {
    Rig rig(server);
    const auto read = [&](int id, const std::string& uri) {
        return Peer::make_request(*modern, id, "resources/read", json{{"uri", uri}});
    };
    expect_scope_refusal(rig.send(read(1, "res://secret"), grant({"docs:read"})), {"secret:read"});
    expect_scope_refusal(rig.send(read(2, "doc://a"), grant({"secret:read"})), {"docs:read"});
    EXPECT_EQ(counts.secret, 0);
    EXPECT_EQ(counts.doc, 0);
    expect_result(rig.send(read(3, "res://secret"), grant({"secret:read"})));
    expect_result(rig.send(read(4, "doc://a"), grant({"docs:read"})));
    EXPECT_EQ(counts.secret, 1);
    EXPECT_EQ(counts.doc, 1);
    // A URI nothing serves has no requirement: it gets the answer for a resource that does not exist.
    const neomcp::Output nothing = rig.send(read(5, "none://x"), grant({}));
    EXPECT_EQ(nothing.refusal, neomcp::RefusalReason::none);
    EXPECT_TRUE(body_of(nothing).contains("error"));
}

TEST_F(ServerAuthorization, APromptAndACompletionNeedTheScopesOfTheirItem) {
    Rig rig(server);
    const json get = Peer::make_request(*modern, 1, "prompts/get",
                                        json{{"name", "plan"}, {"arguments", json{{"topic", "t"}}}});
    expect_scope_refusal(rig.send(get, grant({})), {"plans:read"});
    EXPECT_EQ(counts.plan, 0);
    const auto complete = [&](int id, json ref) {
        return Peer::make_request(*modern, id, "completion/complete",
                                  json{{"ref", std::move(ref)}, {"argument", json{{"name", "x"}, {"value", ""}}}});
    };
    const json prompt_ref{{"type", "ref/prompt"}, {"name", "plan"}};
    const json template_ref{{"type", "ref/resource"}, {"uri", "doc://{name}"}};
    expect_scope_refusal(rig.send(complete(2, prompt_ref), grant({"docs:read"})), {"plans:read"});
    expect_scope_refusal(rig.send(complete(3, template_ref), grant({"plans:read"})), {"docs:read"});
    EXPECT_EQ(counts.completed, 0);

    expect_result(rig.send(get, grant({"plans:read"})));
    json topic = complete(4, prompt_ref);
    topic["params"]["argument"]["name"] = "topic";
    expect_result(rig.send(topic, grant({"plans:read"})));
    json name = complete(5, template_ref);
    name["params"]["argument"]["name"] = "name";
    expect_result(rig.send(name, grant({"docs:read"})));
    EXPECT_EQ(counts.plan, 1);
    EXPECT_EQ(counts.completed, 2);
}

TEST_F(ServerAuthorization, BeingToldOfAResourcesUpdatesNeedsWhatReadingItNeeds) {
    Rig rig(server);
    const auto legacy = builtin("2025-11-25");
    expect_result(rig.send(Peer::initialize_request(*legacy, 1), grant({})));
    rig.send(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, grant({}));
    const auto request = [](int id, const char* method, const char* uri) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", json{{"uri", uri}}}};
    };
    expect_scope_refusal(rig.send(request(2, "resources/subscribe", "res://secret"), grant({})), {"secret:read"});
    expect_scope_refusal(rig.send(request(3, "resources/subscribe", "doc://a"), grant({})), {"docs:read"});
    expect_scope_refusal(rig.send(request(4, "resources/unsubscribe", "res://secret"), grant({})), {"secret:read"});
    expect_result(rig.send(request(5, "resources/subscribe", "res://secret"), grant({"secret:read"})));
    expect_result(rig.send(request(6, "resources/unsubscribe", "res://secret"), grant({"secret:read"})));
}

// A subscription that names resources needs what reading each of them needs (one that names none is told of no
// resource's updates). Only refusals are checked here: an accepted subscription has no last output until it ends.
TEST_F(ServerAuthorization, ASubscriptionThatNamesResourcesNeedsWhatReadingThemNeeds) {
    Rig rig(server);
    const auto listen_to = [&](int id, json uris) {
        return Peer::make_request(*modern, id, "subscriptions/listen",
                                  json{{"notifications", json{{"resourceSubscriptions", std::move(uris)}}}});
    };
    expect_scope_refusal(rig.send(listen_to(1, {"res://secret", "doc://a", "res://secret"}), grant({})),
                         {"secret:read", "docs:read"});
    expect_scope_refusal(rig.send(listen_to(2, {"doc://a", "res://secret"}), grant({"secret:read"})), {"docs:read"});
    EXPECT_EQ(counts.secret, 0);
    EXPECT_EQ(counts.doc, 0);
}

TEST_F(ServerAuthorization, ABatchThatLacksAScopeForOneRequestRunsNoneAndNamesWhatEachLacks) {
    Rig rig(server);
    const auto legacy = builtin("2025-03-26");
    expect_result(rig.send(Peer::initialize_request(*legacy, 1), grant({})));
    rig.send(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, grant({}));
    const auto item = [](int id, const std::string& method, json params) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};
    };
    const json write = item(2, "tools/call", json{{"name", "write"}, {"arguments", json::object()}});
    const json admin = item(3, "tools/call", json{{"name", "admin_only"}, {"arguments", json::object()}});
    const json plan = item(4, "prompts/get", json{{"name", "plan"}, {"arguments", json{{"topic", "t"}}}});

    // The first request has what it needs, the second does not: neither runs.
    neomcp::Output refused = rig.send(json::array({write, admin}), grant({"files:write"}));
    expect_scope_refusal(refused, {"admin"});
    EXPECT_TRUE(body_of(refused).at("id").is_null()) << refused.line;  // it answers the batch, no one request
    EXPECT_EQ(counts.write, 0);
    EXPECT_EQ(counts.admin, 0);

    // What every request lacks, in the order first met, each once.
    expect_scope_refusal(rig.send(json::array({admin, plan, write, admin}), grant({})),
                         {"admin", "plans:read", "files:write"});
    EXPECT_EQ(counts.write + counts.admin + counts.plan, 0);

    const neomcp::Output answered = rig.send(json::array({write, plan}), grant({"files:write", "plans:read"}));
    EXPECT_EQ(answered.refusal, neomcp::RefusalReason::none);
    EXPECT_TRUE(body_of(answered).is_array()) << answered.line;
    EXPECT_EQ(counts.write, 1);
    EXPECT_EQ(counts.plan, 1);
}

TEST_F(ServerAuthorization, PlainAndCoroutineHandlersSeeTheAuthorizationAndNoToken) {
    Rig rig(server);
    expect_result(rig.send(call(*modern, 1, "who"), grant({"x", "y"}, "bob")));
    expect_result(rig.send(call(*modern, 2, "who_async"), grant({"z"}, "carol")));
    ASSERT_TRUE(plain.present);
    EXPECT_EQ(plain.issuer, "https://as.example");
    EXPECT_EQ(plain.subject, std::optional<std::string>("bob"));
    EXPECT_EQ(plain.client_id, std::optional<std::string>("client-1"));
    EXPECT_EQ(plain.scopes, (Scopes{"x", "y"}));
    ASSERT_TRUE(coroutine.present);
    EXPECT_EQ(coroutine.subject, std::optional<std::string>("carol"));
    EXPECT_EQ(coroutine.scopes, Scopes{"z"});
}

TEST_F(ServerAuthorization, ARetryIsAcceptedOnlyUnderThePrincipalOfTheCallThatAsked) {
    Rig rig(server);
    int id = 0;
    const auto first = [&](const Granted& who) {
        const neomcp::Output asked = rig.send(call_with_forms(*modern, ++id, "confirm"), who);
        const json body = body_of(asked);
        EXPECT_EQ(body.at("result").at("resultType"), "input_required") << asked.line;
        return body.at("result").at("requestState");
    };
    const auto retry = [&](const json& state, const Granted& who) {
        json message = call_with_forms(*modern, ++id, "confirm");
        message["params"]["inputResponses"] = json{{"confirm", json{{"action", "accept"}, {"content", json{{"ok", true}}}}}};
        message["params"]["requestState"] = state;
        return body_of(rig.send(message, who));
    };
    const auto refused = [](const json& body) {
        return body.contains("error") && body.at("error").at("data").at("reason") == "invalid_request_state";
    };
    const json state = first(grant({}, "alice"));
    EXPECT_TRUE(refused(retry(state, grant({}, "mallory"))));
    EXPECT_TRUE(refused(retry(state, nullptr)));
    EXPECT_TRUE(refused(retry(state, grant({}, "alice", "client-2"))));
    // The same principal with another token (other scopes, another object) goes on.
    EXPECT_EQ(retry(state, grant({"files:write"}, "alice")).at("result").at("content").at(0).at("text"), "confirmed");

    // A state issued without an authorization comes back without one only.
    const json open_state = first(nullptr);
    EXPECT_TRUE(refused(retry(open_state, grant({}, "alice"))));
    EXPECT_EQ(retry(open_state, nullptr).at("result").at("content").at(0).at("text"), "confirmed");
    EXPECT_EQ(counts.confirm, 4);  // two first rounds and two accepted retries
}

// A host that hands every call to the server but declares no scopes, as a session would see a server whose item was
// switched between its look and the server's: the server itself does not run what the authorization does not meet.
class Undeclared final : public neomcp::ToolHost {
public:
    explicit Undeclared(neomcp::Server& server) : server_(server) {}
    std::span<const neomcp::ToolDefinition> tools() const override { return server_.tools(); }
    bool tool_enabled(std::size_t index) const override { return server_.tool_enabled(index); }
    void call(const neomcp::CallRequest& request, neomcp::CallControl control, neomcp::Completion done) override {
        server_.call(request, std::move(control), std::move(done));
    }

private:
    neomcp::Server& server_;
};

TEST_F(ServerAuthorization, TheServerChecksAgainWhereTheSessionFoundNoDeclaration) {
    Undeclared host(server);
    std::mutex mutex;
    std::condition_variable arrived;
    std::map<neomcp::UnitId, neomcp::Output> last;
    auto made = neomcp::Session::create_tagged(neomcp::SessionOptions{}, host, [&](neomcp::Output output) {
        const std::lock_guard lock(mutex);
        if (output.unit && output.last) last.emplace(*output.unit, std::move(output));
        arrived.notify_all();
    });
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<neomcp::Session>>(made));
    auto& session = *std::get<std::unique_ptr<neomcp::Session>>(made);
    session.receive(1, neomcp::serialize(call(*modern, 1, "write")), {}, grant({}));
    session.receive(2, neomcp::serialize(call(*modern, 2, "write")), {}, grant({"files:write"}));
    std::unique_lock lock(mutex);
    ASSERT_TRUE(arrived.wait_for(lock, patience, [&] { return last.size() == 2; }));
    const json refused = json::parse(last.at(1).line);
    EXPECT_EQ(last.at(1).refusal, neomcp::RefusalReason::none);
    EXPECT_EQ(refused.at("error").at("code"), -32603) << last.at(1).line;
    EXPECT_TRUE(json::parse(last.at(2).line).contains("result")) << last.at(2).line;
    EXPECT_EQ(counts.write, 1);
    lock.unlock();
    session.close();
}

TEST(ServerAuthorizationRegistration, ScopesAreScopeTokensAndFixedOnceTheServerServes) {
    neomcp::Server server({.name = "s", .version = "1"});
    const auto tool = server.tool("t", "a tool", [] { return std::string("t"); });
    EXPECT_FALSE(server.declares_scopes());
    for (const char* bad : {"", "a b", "quote\"", "back\\slash", "tab\t"}) {
        try {
            tool.scopes({"ok", bad});
            ADD_FAILURE() << "accepted '" << bad << "'";
        } catch (const std::invalid_argument& error) {
            EXPECT_NE(std::string(error.what()).find("tool 't'"), std::string::npos) << error.what();
        }
    }
    EXPECT_FALSE(server.declares_scopes());
    tool.scopes({"files:read", "!#$%&'()*+,-./:;<=>?@[]^_`{|}~"});
    EXPECT_TRUE(server.declares_scopes());
    static_cast<void>(server.tools());  // serving begins
    EXPECT_THROW(tool.scopes({"other"}), std::logic_error);
    EXPECT_EQ(server.tool_scopes(0).size(), 2u);
}

}  // namespace
