// Tests of the client halves of the mechanisms: each half against the server half of the same name on every profile
// that names it, and against what a real Session writes, so that what the client writes is what the server reads and
// what the client reads is what the server writes. Plus the registry, the selection of the halves of a profile, and
// malformed messages, which give typed errors and never throw.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/client/mechanism.h"
#include "neomcp/mechanism.h"
#include "neomcp/session.h"
#include "neomcp/testing/peer.h"
#include "neomcp/tool_host.h"
#include "neomcp/wire.h"

namespace {

using namespace std::chrono_literals;
using neomcp::json;
using neomcp::testing::Peer;
namespace profile = neomcp::profile;

constexpr std::chrono::milliseconds patience = 30s;

json J(const char* text) { return json::parse(text); }

const neomcp::ClientMechanismRegistry& client_registry() { return neomcp::builtin_client_mechanisms(); }

std::vector<profile::ProfilePtr> all_profiles() {
    std::vector<profile::ProfilePtr> out;
    for (const auto revision : profile::builtin_revisions()) out.push_back(profile::builtin(revision));
    return out;
}

// The profiles newest first, as a client engine orders its candidates.
std::vector<profile::ProfilePtr> newest_first() {
    auto out = all_profiles();
    std::reverse(out.begin(), out.end());
    return out;
}

std::unique_ptr<neomcp::ClientOpenMechanism> client_open(const profile::Profile& p) {
    return client_registry().make_open(p.mechanism(profile::Slot::open));
}
std::unique_ptr<neomcp::ClientInputMechanism> client_input(const profile::Profile& p) {
    return client_registry().make_input(p.mechanism(profile::Slot::server_input));
}
std::unique_ptr<neomcp::ClientFeedMechanism> client_feed(const profile::Profile& p) {
    return client_registry().make_feed(p.mechanism(profile::Slot::change_feed));
}

// The profiles whose open half handshakes, newest first: the candidates of a handshake.
std::vector<profile::ProfilePtr> handshake_candidates() {
    std::vector<profile::ProfilePtr> out;
    for (const auto& p : newest_first()) {
        if (client_open(*p)->handshakes()) out.push_back(p);
    }
    return out;
}

// What the test client says about itself under a profile.
neomcp::ClientSelf self_for(const profile::Profile& p, std::optional<neomcp::LogLevel> level = std::nullopt) {
    neomcp::ClientSelf self;
    self.info.name = "test-client";
    self.info.version = "1.2.3";
    self.info.title = "Test client";
    json capabilities = J(R"({"elicitation":{"form":{}},"sampling":{},"roots":{}})");
    neomcp::keep_members(capabilities, "ClientCapabilities", p);
    self.capabilities = std::move(capabilities);
    self.log_level = level;
    return self;
}

json frame_request(const neomcp::RequestId& id, const neomcp::ClientMessage& message) {
    json framed = json::object();
    framed["jsonrpc"] = "2.0";
    framed["id"] = id.to_json();
    framed["method"] = message.method;
    if (!message.params.is_null()) framed["params"] = message.params;
    return framed;
}

json frame_notification(const neomcp::ClientMessage& message) {
    json framed = json::object();
    framed["jsonrpc"] = "2.0";
    framed["method"] = message.method;
    if (!message.params.is_null()) framed["params"] = message.params;
    return framed;
}

// A request of the client under a profile: annotated by the profile's open half, framed.
json client_request(const profile::Profile& p, const neomcp::RequestId& id, std::string method, json params,
                    const neomcp::ClientSelf& self) {
    const auto open = client_open(p);
    if (auto problem = open->annotate(params, p, self)) throw std::logic_error(*problem);
    return frame_request(id, neomcp::ClientMessage{std::move(method), std::move(params)});
}

json result_of(const std::optional<json>& answer) {
    if (!answer) throw std::runtime_error("no answer within the patience");
    if (!answer->contains("result")) throw std::runtime_error("an error answer: " + answer->dump());
    return answer->at("result");
}

// ---------------------------------------------------------------------------------------------------------------
// A host whose tool asks for input, and whose changes the test reports.

class AskingHost final : public neomcp::ToolHost {
public:
    AskingHost() {
        neomcp::ToolDefinition asks;
        asks.name = "asks";
        asks.description = "Asks for what the client supports, then answers";
        tools_.push_back(asks);
        neomcp::ToolDefinition plain;
        plain.name = "plain";
        plain.description = "Answers at once";
        tools_.push_back(plain);
    }

    std::span<const neomcp::ToolDefinition> tools() const override { return tools_; }
    neomcp::ChangeSupport change_support() const override {
        neomcp::ChangeSupport support;
        support.tools_list = true;
        return support;
    }
    neomcp::ChangeRegistration watch(std::weak_ptr<neomcp::ChangeListener> listener) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        listeners_.push_back(std::move(listener));
        return neomcp::ChangeRegistration([] {});
    }
    // Tells every listener of the change; returns how many were told.
    int report(const neomcp::Change& change) {
        std::vector<std::shared_ptr<neomcp::ChangeListener>> told;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& weak : listeners_) {
                if (auto strong = weak.lock()) told.push_back(std::move(strong));
            }
        }
        for (const auto& listener : told) listener->changed(change);
        return static_cast<int>(told.size());
    }

    void call(const neomcp::CallRequest& request, neomcp::CallControl control, neomcp::Completion done) override {
        if (request.name == "asks" && request.input_responses.empty()) {
            neomcp::InputRequired ask;
            const neomcp::InputRequest form = neomcp::ElicitationForm{
                "Go on?", J(R"({"type":"object","properties":{"ok":{"type":"boolean"}},"required":["ok"]})")};
            if (control.client_supports(form)) ask.requests.emplace("confirm", form);
            const neomcp::InputRequest roots = neomcp::RootsRequest{};
            if (control.client_supports(roots)) ask.requests.emplace("roots", roots);
            neomcp::SamplingRequest sampling;
            neomcp::TextContent hello;
            hello.text = "Hello";
            sampling.messages.push_back(neomcp::SamplingMessage{neomcp::Role::user, {std::move(hello)}});
            sampling.max_tokens = 16;
            const neomcp::InputRequest sample = std::move(sampling);
            if (control.client_supports(sample)) ask.requests.emplace("sample", sample);
            ask.state = "round-one";
            done(neomcp::CallOutcome(std::move(ask)));
            return;
        }
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            last_ = request;
        }
        std::string text = request.name + ":";
        for (const auto& [key, answer] : request.input_responses) text += key + ",";
        neomcp::ToolResult result;
        neomcp::TextContent block;
        block.text = std::move(text);
        result.content.emplace_back(std::move(block));
        done(neomcp::CallOutcome(std::move(result)));
    }

    std::optional<neomcp::CallRequest> last() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return last_;
    }

private:
    std::vector<neomcp::ToolDefinition> tools_;
    mutable std::mutex mutex_;
    std::vector<std::weak_ptr<neomcp::ChangeListener>> listeners_;
    std::optional<neomcp::CallRequest> last_;
};

neomcp::SessionOptions session_options(bool logging = false) {
    neomcp::SessionOptions options;
    options.server_info.name = "test-server";
    options.server_info.version = "9.9.9";
    options.protocol_logging = logging;
    return options;
}

// The answers a client gives to what the host asks.
neomcp::InputResponse answer_for(const neomcp::InputRequest& request) {
    if (std::holds_alternative<neomcp::ElicitationForm>(request)) {
        return neomcp::ElicitationAnswer{neomcp::ElicitationAction::accept, J(R"({"ok":true})")};
    }
    if (std::holds_alternative<neomcp::SamplingRequest>(request)) {
        neomcp::SamplingAnswer answer;
        answer.content = J(R"({"type":"text","text":"Hi"})");
        answer.model = "test-model";
        return answer;
    }
    neomcp::RootsAnswer roots;
    roots.roots.push_back(neomcp::Root{"file:///work", std::string("work"), std::nullopt});
    return roots;
}

// The handshake the client half builds, sent to the session; returns the accepted handshake and leaves the
// connection opened.
neomcp::HandshakeAccepted open_connection(Peer& peer, const profile::ProfilePtr& p, const neomcp::ClientSelf& self) {
    const auto open = client_open(*p);
    const auto handshake = open->handshake(*p, self);
    if (!handshake) throw std::logic_error("no handshake for " + p->revision());
    const json result = result_of(peer.request(frame_request(neomcp::RequestId(1), *handshake), patience));
    const auto candidates = handshake_candidates();
    auto read = open->read_handshake(result, candidates);
    auto* accepted = std::get_if<neomcp::HandshakeAccepted>(&read);
    if (accepted == nullptr) throw std::runtime_error("the handshake result was not read for " + p->revision());
    const auto opened = open->opened(*p);
    if (!opened) throw std::logic_error("no ready notification for " + p->revision());
    peer.notify(frame_notification(*opened));
    return *accepted;
}

// ---------------------------------------------------------------------------------------------------------------
// The registry and the selection of halves

TEST(ClientMechanismRegistry, HasAClientHalfForEveryDeclaredName) {
    const auto& registry = client_registry();
    const struct {
        profile::Slot slot;
        neomcp::MechanismSlot mechanism_slot;
    } slots[] = {{profile::Slot::open, neomcp::MechanismSlot::open},
                 {profile::Slot::state, neomcp::MechanismSlot::state},
                 {profile::Slot::server_input, neomcp::MechanismSlot::server_input},
                 {profile::Slot::change_feed, neomcp::MechanismSlot::change_feed},
                 {profile::Slot::batch, neomcp::MechanismSlot::batch}};
    for (const auto& entry : slots) {
        for (const auto name : profile::declared_mechanisms(entry.slot)) {
            EXPECT_TRUE(registry.contains(entry.mechanism_slot, name)) << name;
        }
        const auto declared = profile::declared_mechanisms(entry.slot);
        EXPECT_EQ(registry.names(entry.mechanism_slot).size(), declared.size());
    }
    for (const auto transport :
         {profile::Transport::stdio, profile::Transport::http_sse, profile::Transport::streamable_http}) {
        for (const auto name : profile::declared_cancel(transport)) {
            EXPECT_TRUE(registry.contains(neomcp::MechanismSlot::cancel, name)) << name;
            EXPECT_NE(registry.make_cancel(name), nullptr) << name;
        }
    }
    for (const auto name : profile::declared_mechanisms(profile::Slot::open)) EXPECT_NE(registry.make_open(name), nullptr);
    for (const auto name : profile::declared_mechanisms(profile::Slot::state)) EXPECT_NE(registry.make_state(name), nullptr);
    for (const auto name : profile::declared_mechanisms(profile::Slot::batch)) EXPECT_NE(registry.make_batch(name), nullptr);
    for (const auto name : profile::declared_mechanisms(profile::Slot::server_input)) {
        EXPECT_NE(registry.make_input(name), nullptr);
    }
    for (const auto name : profile::declared_mechanisms(profile::Slot::change_feed)) {
        EXPECT_NE(registry.make_feed(name), nullptr);
    }
}

TEST(ClientMechanismRegistry, TheStateAndInputHalvesAgreeWithTheServerHalves) {
    for (const auto name : profile::declared_mechanisms(profile::Slot::state)) {
        EXPECT_EQ(client_registry().make_state(name)->keeps_connection(),
                  neomcp::builtin_mechanisms().make_state(name)->keeps_connection())
            << name;
    }
    for (const auto name : profile::declared_mechanisms(profile::Slot::server_input)) {
        EXPECT_EQ(client_registry().make_input(name)->delivery(), neomcp::builtin_mechanisms().make_input(name)->delivery())
            << name;
    }
    for (const auto& p : all_profiles()) {
        const auto name = p->mechanism(profile::Slot::server_input);
        const auto client = client_registry().make_input(name);
        const auto server = neomcp::builtin_mechanisms().make_input(name);
        for (const auto& request : p->requests()) {
            EXPECT_EQ(client->asks_on(*p, request.method), server->asks_on(*p, request.method))
                << p->revision() << " " << request.method;
        }
    }
}

TEST(ClientMechanismRegistry, EveryOpenHalfHandshakesOrNamesTheRevisionPerRequest) {
    for (const auto name : profile::declared_mechanisms(profile::Slot::open)) {
        const auto open = client_registry().make_open(name);
        ASSERT_NE(open, nullptr) << name;
        EXPECT_NE(open->handshakes(), open->names_revision_per_request()) << name;
    }
}

TEST(ClientMechanismRegistry, SelectsTheHalvesOfEveryProfileThatDefinesTheTransport) {
    const auto profiles = all_profiles();
    for (const auto transport :
         {profile::Transport::stdio, profile::Transport::http_sse, profile::Transport::streamable_http}) {
        auto selected = neomcp::select_client_mechanisms(profiles, transport);
        ASSERT_TRUE(std::holds_alternative<std::vector<neomcp::ClientMechanisms>>(selected))
            << std::get<neomcp::ClientMechanismsError>(selected).message;
        const auto& halves = std::get<std::vector<neomcp::ClientMechanisms>>(selected);
        std::size_t defining = 0;
        for (const auto& p : profiles) {
            const bool defines = transport == profile::Transport::stdio ? p->stdio().has_value()
                                 : transport == profile::Transport::http_sse ? p->http_sse().has_value()
                                                                              : p->streamable_http().has_value();
            if (defines) ++defining;
        }
        ASSERT_EQ(halves.size(), defining);
        for (const auto& one : halves) {
            EXPECT_NE(one.open, nullptr);
            EXPECT_NE(one.state, nullptr);
            EXPECT_NE(one.batch, nullptr);
            EXPECT_NE(one.cancel, nullptr);
            EXPECT_NE(one.server_input, nullptr);
            EXPECT_NE(one.change_feed, nullptr);
        }
    }
}

TEST(ClientMechanismRegistry, AMissingHalfNamesTheRevisionTheSlotAndTheName) {
    const auto modern = profile::builtin("2026-07-28");
    ASSERT_NE(modern, nullptr);
    neomcp::ClientMechanismRegistry registry = client_registry();
    const std::string open_name(modern->mechanism(profile::Slot::open));
    const std::string cancel_name = modern->streamable_http()->cancel;
    ASSERT_TRUE(registry.remove(neomcp::MechanismSlot::open, open_name));
    ASSERT_TRUE(registry.remove(neomcp::MechanismSlot::cancel, cancel_name));
    EXPECT_FALSE(registry.remove(neomcp::MechanismSlot::open, open_name));

    const std::vector<profile::ProfilePtr> profiles{modern};
    auto selected = neomcp::select_client_mechanisms(profiles, profile::Transport::streamable_http, registry);
    ASSERT_TRUE(std::holds_alternative<neomcp::ClientMechanismsError>(selected));
    const auto& error = std::get<neomcp::ClientMechanismsError>(selected);
    ASSERT_EQ(error.missing.size(), 2U) << error.message;
    EXPECT_EQ(error.missing[0].revision, "2026-07-28");
    EXPECT_EQ(error.missing[0].slot, neomcp::MechanismSlot::open);
    EXPECT_EQ(error.missing[0].name, open_name);
    EXPECT_EQ(error.missing[1].revision, "2026-07-28");
    EXPECT_EQ(error.missing[1].slot, neomcp::MechanismSlot::cancel);
    EXPECT_EQ(error.missing[1].name, cancel_name);
    EXPECT_NE(error.message.find(open_name), std::string::npos);
    EXPECT_NE(error.message.find('\n'), std::string::npos);

    // A profile that does not define the transport is left out, so its halves are not asked for.
    const std::vector<profile::ProfilePtr> oldest{profile::builtin("2024-11-05")};
    auto none = neomcp::select_client_mechanisms(oldest, profile::Transport::streamable_http, registry);
    ASSERT_TRUE(std::holds_alternative<std::vector<neomcp::ClientMechanisms>>(none));
    EXPECT_TRUE(std::get<std::vector<neomcp::ClientMechanisms>>(none).empty());

    const std::vector<profile::ProfilePtr> null_profile{nullptr};
    EXPECT_TRUE(std::holds_alternative<neomcp::ClientMechanismsError>(
        neomcp::select_client_mechanisms(null_profile, profile::Transport::stdio)));
}

// ---------------------------------------------------------------------------------------------------------------
// open: the handshake

TEST(ClientMechanismOpen, TheHandshakeIsAcceptedByTheServerHalfAndItsResultIsRead) {
    const auto candidates = handshake_candidates();
    ASSERT_FALSE(candidates.empty());
    for (const auto& p : candidates) {
        SCOPED_TRACE(p->revision());
        const auto open = client_open(*p);
        const auto self = self_for(*p);
        const auto handshake = open->handshake(*p, self);
        ASSERT_TRUE(handshake.has_value());

        // The server half reads it as a handshake at this revision, with the client as written.
        const auto server = neomcp::builtin_mechanisms().make_open(p->mechanism(profile::Slot::open));
        const auto request = neomcp::classify(frame_request(neomcp::RequestId(7), *handshake));
        ASSERT_TRUE(server->opens_connection(request));
        auto opened = server->open(request, candidates);
        ASSERT_TRUE(std::holds_alternative<neomcp::Handshake>(opened)) << std::get<neomcp::RequestError>(opened).message;
        const auto& handshake_read = std::get<neomcp::Handshake>(opened);
        EXPECT_EQ(handshake_read.profile->revision(), p->revision());
        EXPECT_EQ(handshake_read.client.info, neomcp::to_wire(self.info, *p));
        EXPECT_EQ(handshake_read.client.capabilities, self.capabilities);

        // A session answers it, and the client reads the result it writes.
        AskingHost host;
        Peer peer(host, session_options());
        const auto accepted = open_connection(peer, p, self);
        EXPECT_EQ(accepted.profile->revision(), p->revision());
        ASSERT_TRUE(accepted.server.info.is_object());
        EXPECT_EQ(accepted.server.info.at("name"), "test-server");
        EXPECT_TRUE(accepted.server.capabilities.is_object());
        EXPECT_TRUE(accepted.server.result.is_object());
        // The connection is open: a request after it is served.
        const json listed = result_of(peer.request(frame_request(neomcp::RequestId(2), {"tools/list", nullptr}), patience));
        EXPECT_EQ(listed.at("tools").size(), 2U);

        EXPECT_TRUE(open->allowed_before_open("ping"));
        EXPECT_FALSE(open->allowed_before_open("tools/list"));
        EXPECT_FALSE(open->probe(*p).has_value());
        // A handshake adds nothing to a request.
        json params = J(R"({"name":"x"})");
        EXPECT_FALSE(open->annotate(params, *p, self).has_value());
        EXPECT_EQ(params, J(R"({"name":"x"})"));
    }
}

TEST(ClientMechanismOpen, AHandshakeResultOfAnotherRevisionIsNamedAsData) {
    const auto candidates = handshake_candidates();
    const auto open = client_open(*candidates.front());
    const json result = J(R"({"protocolVersion":"1999-01-01","capabilities":{},"serverInfo":{"name":"s","version":"1"}})");
    auto read = open->read_handshake(result, candidates);
    ASSERT_TRUE(std::holds_alternative<neomcp::UnservedRevision>(read));
    EXPECT_EQ(std::get<neomcp::UnservedRevision>(read).revision, "1999-01-01");

    // A result without what the revision requires, or with members of the wrong kind, is malformed.
    for (const char* text : {R"({"protocolVersion":"2025-11-25"})",
                             R"({"protocolVersion":"2025-11-25","capabilities":5,"serverInfo":{"name":"s","version":"1"}})",
                             R"({"protocolVersion":"2025-11-25","capabilities":{},"serverInfo":[]})",
                             R"({"protocolVersion":7})", R"([1,2])", R"("text")"}) {
        EXPECT_TRUE(std::holds_alternative<std::string>(open->read_handshake(J(text), candidates))) << text;
    }
}

TEST(ClientMechanismOpen, AHandshakeRefusalIsReadForItsSupportedList) {
    const auto p = handshake_candidates().front();
    const auto open = client_open(*p);
    auto read = open->read_version_refusal(
        J(R"({"code":-32602,"message":"Unsupported protocol version","data":{"supported":["2024-11-05"],"requested":"1.0.0"}})"), *p);
    ASSERT_TRUE(read.has_value());
    ASSERT_TRUE(std::holds_alternative<neomcp::VersionRefusal>(*read));
    EXPECT_EQ(std::get<neomcp::VersionRefusal>(*read).supported, std::vector<std::string>{"2024-11-05"});
    EXPECT_EQ(std::get<neomcp::VersionRefusal>(*read).requested, std::optional<std::string>("1.0.0"));
    // Another error, or one without a list, is no refusal; a list of the wrong kind is malformed.
    EXPECT_FALSE(open->read_version_refusal(J(R"({"code":-32601,"message":"x"})"), *p).has_value());
    EXPECT_FALSE(open->read_version_refusal(J(R"({"code":-32602,"message":"x"})"), *p).has_value());
    // A code beyond the range of int never wraps into the refusal's code (-32602 + 2^32 and -32602 - 2^32).
    EXPECT_FALSE(open->read_version_refusal(J(R"({"code":4294934694,"message":"x","data":{"supported":["a"]}})"), *p)
                     .has_value());
    EXPECT_FALSE(open->read_version_refusal(J(R"({"code":-4294999898,"message":"x","data":{"supported":["a"]}})"), *p)
                     .has_value());
    const auto bad = open->read_version_refusal(J(R"({"code":-32602,"message":"x","data":{"supported":[1]}})"), *p);
    ASSERT_TRUE(bad.has_value());
    EXPECT_TRUE(std::holds_alternative<std::string>(*bad));
}

TEST(ClientMechanismOpen, TheLevelGoesInARequestAfterTheHandshake) {
    for (const auto& p : handshake_candidates()) {
        SCOPED_TRACE(p->revision());
        AskingHost host;
        Peer peer(host, session_options(true));
        const auto accepted = open_connection(peer, p, self_for(*p));
        const auto open = client_open(*p);
        const auto request = open->level_request(neomcp::LogLevel::warning, *p, accepted.server.capabilities);
        ASSERT_TRUE(request.has_value());
        result_of(peer.request(frame_request(neomcp::RequestId(3), *request), patience));
        // A server that declares no logging gets no such request.
        EXPECT_FALSE(open->level_request(neomcp::LogLevel::warning, *p, json::object()).has_value());
    }
}

// ---------------------------------------------------------------------------------------------------------------
// open: the revision in every request

std::vector<profile::ProfilePtr> per_request_profiles() {
    std::vector<profile::ProfilePtr> out;
    for (const auto& p : newest_first()) {
        if (client_open(*p)->names_revision_per_request()) out.push_back(p);
    }
    return out;
}

TEST(ClientMechanismOpen, TheMetaOfEveryRequestIsAcceptedByTheServerHalf) {
    const auto profiles = per_request_profiles();
    ASSERT_FALSE(profiles.empty());
    for (const auto& p : profiles) {
        SCOPED_TRACE(p->revision());
        const auto open = client_open(*p);
        const auto server = neomcp::builtin_mechanisms().make_open(p->mechanism(profile::Slot::open));
        const auto self = self_for(*p, neomcp::LogLevel::warning);
        const auto request = neomcp::classify(client_request(*p, neomcp::RequestId(1), "tools/list", nullptr, self));
        auto named = server->named_revision(request);
        ASSERT_TRUE(named.has_value());
        ASSERT_TRUE(std::holds_alternative<std::string>(*named));
        EXPECT_EQ(std::get<std::string>(*named), p->revision());
        auto scope = server->scope(request, *p, true);
        ASSERT_TRUE(std::holds_alternative<neomcp::RequestScope>(scope)) << std::get<neomcp::RequestError>(scope).message;
        const auto& read = std::get<neomcp::RequestScope>(scope);
        EXPECT_EQ(read.client.capabilities, self.capabilities);
        EXPECT_EQ(read.client.info, neomcp::to_wire(self.info, *p));
        EXPECT_EQ(read.log_level, std::optional<neomcp::LogLevel>(neomcp::LogLevel::warning));

        // A session serves it.
        AskingHost host;
        Peer peer(host, session_options());
        const json listed = result_of(peer.request(client_request(*p, neomcp::RequestId(2), "tools/list", nullptr, self), patience));
        EXPECT_EQ(listed.at("tools").size(), 2U);

        EXPECT_FALSE(open->handshake(*p, self).has_value());
        EXPECT_FALSE(open->opened(*p).has_value());
        EXPECT_FALSE(open->level_request(neomcp::LogLevel::warning, *p, J(R"({"logging":{}})")).has_value());
    }
}

TEST(ClientMechanismOpen, TheMetaKeepsWhatIsNotItsOwnAndRewritesItsOwnKeys) {
    for (const auto& p : per_request_profiles()) {
        const auto open = client_open(*p);
        json params = J(R"({"name":"x","_meta":{"progressToken":"p1","io.modelcontextprotocol/protocolVersion":"old",
                           "io.modelcontextprotocol/logLevel":"debug"}})");
        ASSERT_FALSE(open->annotate(params, *p, self_for(*p)).has_value());
        const json& meta = params.at("_meta");
        EXPECT_EQ(meta.at("progressToken"), "p1");
        EXPECT_EQ(meta.at("io.modelcontextprotocol/protocolVersion"), p->revision());
        EXPECT_FALSE(meta.contains("io.modelcontextprotocol/logLevel"));
        EXPECT_EQ(params.at("name"), "x");

        json bad = J("[1]");
        EXPECT_TRUE(open->annotate(bad, *p, self_for(*p)).has_value());
        json bad_meta = J(R"({"_meta":5})");
        EXPECT_TRUE(open->annotate(bad_meta, *p, self_for(*p)).has_value());
    }
}

TEST(ClientMechanismOpen, TheProbeIsAnsweredByASessionAndItsResultIsRead) {
    for (const auto& p : per_request_profiles()) {
        SCOPED_TRACE(p->revision());
        const auto open = client_open(*p);
        const auto self = self_for(*p);
        const auto probe = open->probe(*p);
        ASSERT_TRUE(probe.has_value());
        AskingHost host;
        Peer peer(host, session_options());
        const json result = result_of(peer.request(client_request(*p, neomcp::RequestId(1), probe->method, probe->params, self), patience));
        const auto read = open->read_discovery(result, *p);
        ASSERT_TRUE(read.has_value());
        ASSERT_TRUE(std::holds_alternative<neomcp::Discovery>(*read)) << std::get<std::string>(*read);
        const auto& discovery = std::get<neomcp::Discovery>(*read);
        EXPECT_NE(std::find(discovery.versions.begin(), discovery.versions.end(), p->revision()), discovery.versions.end());
        ASSERT_TRUE(discovery.server.info.is_object());
        EXPECT_EQ(discovery.server.info.at("name"), "test-server");
        EXPECT_TRUE(discovery.server.capabilities.is_object());

        // A legacy server's empty result is not a discovery result; one with the members but a malformed value is an
        // error.
        EXPECT_FALSE(open->read_discovery(json::object(), *p).has_value());
        const auto bad = open->read_discovery(
            J(R"({"cacheScope":"public","capabilities":{},"resultType":"complete","supportedVersions":"x","ttlMs":0})"), *p);
        ASSERT_TRUE(bad.has_value());
        EXPECT_TRUE(std::holds_alternative<std::string>(*bad));
    }
}

TEST(ClientMechanismOpen, ARefusedVersionIsReadFromTheSessionsError) {
    for (const auto& p : per_request_profiles()) {
        SCOPED_TRACE(p->revision());
        const auto open = client_open(*p);
        json request = client_request(*p, neomcp::RequestId(1), "tools/list", nullptr, self_for(*p));
        request["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] = "2099-01-01";
        AskingHost host;
        Peer peer(host, session_options());
        const auto answer = peer.request(request, patience);
        ASSERT_TRUE(answer.has_value());
        ASSERT_TRUE(answer->contains("error")) << answer->dump();
        const auto read = open->read_version_refusal(answer->at("error"), *p);
        ASSERT_TRUE(read.has_value());
        ASSERT_TRUE(std::holds_alternative<neomcp::VersionRefusal>(*read)) << std::get<std::string>(*read);
        const auto& refusal = std::get<neomcp::VersionRefusal>(*read);
        EXPECT_NE(std::find(refusal.supported.begin(), refusal.supported.end(), p->revision()), refusal.supported.end());
        EXPECT_EQ(refusal.requested, std::optional<std::string>("2099-01-01"));
        // The code alone refuses: an answer of it without a list is malformed.
        const int code = answer->at("error").at("code").get<int>();
        json bare = json::object();
        bare["code"] = code;
        bare["message"] = "x";
        const auto malformed = open->read_version_refusal(bare, *p);
        ASSERT_TRUE(malformed.has_value());
        EXPECT_TRUE(std::holds_alternative<std::string>(*malformed));
        EXPECT_FALSE(open->read_version_refusal(J(R"({"code":-32601,"message":"x"})"), *p).has_value());
    }
}

// ---------------------------------------------------------------------------------------------------------------
// server_input: the result asks

std::vector<profile::ProfilePtr> profiles_with_input(neomcp::InputDelivery delivery) {
    std::vector<profile::ProfilePtr> out;
    for (const auto& p : newest_first()) {
        if (client_input(*p)->delivery() == delivery) out.push_back(p);
    }
    return out;
}

TEST(ClientMechanismInput, TheAskingResultIsReadAndTheRetryIsReadBackByTheServer) {
    const auto profiles = profiles_with_input(neomcp::InputDelivery::result);
    ASSERT_FALSE(profiles.empty());
    for (const auto& p : profiles) {
        SCOPED_TRACE(p->revision());
        const auto input = client_input(*p);
        const auto self = self_for(*p);
        AskingHost host;
        Peer peer(host, session_options());
        const json call_params = J(R"({"name":"asks","arguments":{}})");
        const json first = result_of(peer.request(client_request(*p, neomcp::RequestId(1), "tools/call", call_params, self), patience));
        auto read = input->read_result(first, "tools/call", *p);
        ASSERT_TRUE(std::holds_alternative<std::optional<neomcp::InputAsking>>(read)) << std::get<std::string>(read);
        const auto& asking = std::get<std::optional<neomcp::InputAsking>>(read);
        ASSERT_TRUE(asking.has_value());
        ASSERT_EQ(asking->requests.size(), 3U);
        EXPECT_EQ(asking->requests[0].key, "confirm");
        EXPECT_TRUE(std::holds_alternative<neomcp::ElicitationForm>(asking->requests[0].request));
        EXPECT_EQ(asking->requests[1].key, "roots");
        EXPECT_EQ(asking->requests[2].key, "sample");
        EXPECT_EQ(asking->requests[2].method, "sampling/createMessage");
        ASSERT_TRUE(asking->state.has_value());

        neomcp::InputResponses answers;
        for (const auto& asked : asking->requests) answers.emplace(asked.key, answer_for(asked.request));
        // Members of an earlier round in the params are dropped before this round's are written.
        json stale = call_params;
        stale["requestState"] = "stale";
        stale["inputResponses"] = J(R"({"old":{}})");
        auto retry = input->retry_params(stale, answers, asking->state, *p);
        ASSERT_TRUE(std::holds_alternative<json>(retry)) << std::get<std::string>(retry);
        const json retry_message = client_request(*p, neomcp::RequestId(2), "tools/call", std::get<json>(retry), self);
        EXPECT_FALSE(retry_message.at("params").at("inputResponses").contains("old"));
        EXPECT_EQ(retry_message.at("params").at("requestState"), *asking->state);

        // The server half reads back the answers and the state.
        const auto server = neomcp::builtin_mechanisms().make_input(p->mechanism(profile::Slot::server_input));
        auto read_back = server->retry(neomcp::classify(retry_message), *p);
        ASSERT_TRUE(std::holds_alternative<neomcp::InputRetry>(read_back));
        const auto& retried = std::get<neomcp::InputRetry>(read_back);
        ASSERT_TRUE(retried.responses.has_value());
        EXPECT_EQ(retried.responses->size(), 3U);
        EXPECT_EQ(retried.state, asking->state);

        // The session completes the call with them.
        const json done = result_of(peer.request(retry_message, patience));
        ASSERT_EQ(input->read_result(done, "tools/call", *p).index(), 0U);
        EXPECT_FALSE(std::get<std::optional<neomcp::InputAsking>>(input->read_result(done, "tools/call", *p)).has_value());
        const auto last = host.last();
        ASSERT_TRUE(last.has_value());
        EXPECT_EQ(last->input_responses.size(), 3U);
        EXPECT_EQ(last->request_state, std::optional<std::string>("round-one"));
    }
}

TEST(ClientMechanismInput, ResultKindsAreReadStrictly) {
    for (const auto& p : profiles_with_input(neomcp::InputDelivery::result)) {
        const auto input = client_input(*p);
        const auto complete = [&](const char* text, const char* method) {
            auto read = input->read_result(J(text), method, *p);
            return std::holds_alternative<std::optional<neomcp::InputAsking>>(read) &&
                   !std::get<std::optional<neomcp::InputAsking>>(read).has_value();
        };
        const auto error = [&](const char* text, const char* method) {
            return std::holds_alternative<std::string>(input->read_result(J(text), method, *p));
        };
        EXPECT_TRUE(complete(R"({"tools":[]})", "tools/list"));
        EXPECT_TRUE(complete(R"({"resultType":"complete","tools":[]})", "tools/list"));
        EXPECT_TRUE(error(R"({"resultType":"partial"})", "tools/call"));
        EXPECT_TRUE(error(R"({"resultType":5})", "tools/call"));
        // A method that cannot ask, and a result that asks with nothing.
        EXPECT_TRUE(error(R"({"resultType":"input_required","requestState":"s"})", "tools/list"));
        EXPECT_TRUE(error(R"({"resultType":"input_required"})", "tools/call"));
        EXPECT_TRUE(error(R"({"resultType":"input_required","inputRequests":[]})", "tools/call"));
        EXPECT_TRUE(error(R"({"resultType":"input_required","inputRequests":{"a":5}})", "tools/call"));
        EXPECT_TRUE(error(R"({"resultType":"input_required","inputRequests":{"a":{"method":"tools/list"}}})", "tools/call"));
        EXPECT_TRUE(error(R"({"resultType":"input_required","inputRequests":{"a":{"method":"roots/list","params":7}}})",
                          "tools/call"));
        // A state alone asks to be called again at once, and comes back as it was.
        auto state_only = input->read_result(J(R"({"resultType":"input_required","requestState":{"opaque":[1]}})"),
                                             "tools/call", *p);
        ASSERT_TRUE(std::holds_alternative<std::optional<neomcp::InputAsking>>(state_only));
        const auto& asking = std::get<std::optional<neomcp::InputAsking>>(state_only);
        ASSERT_TRUE(asking.has_value());
        EXPECT_TRUE(asking->requests.empty());
        auto retry = input->retry_params(J(R"({"name":"t"})"), {}, asking->state, *p);
        ASSERT_TRUE(std::holds_alternative<json>(retry));
        EXPECT_EQ(std::get<json>(retry), J(R"({"name":"t","requestState":{"opaque":[1]}})"));
        // Without a state the retry carries none, and without answers no answers: not even an earlier round's.
        auto no_state = input->retry_params(J(R"({"name":"t","requestState":"s","inputResponses":{"x":{}}})"), {},
                                            std::nullopt, *p);
        ASSERT_TRUE(std::holds_alternative<json>(no_state));
        EXPECT_FALSE(std::get<json>(no_state).contains("requestState"));
        EXPECT_FALSE(std::get<json>(no_state).contains("inputResponses"));
        EXPECT_TRUE(std::holds_alternative<std::string>(input->retry_params(J("[1]"), {}, std::nullopt, *p)));
    }
}

TEST(ClientMechanismInput, AStateThatCannotBeSentBackUnchangedIsRefused) {
    for (const auto& p : profiles_with_input(neomcp::InputDelivery::result)) {
        const auto input = client_input(*p);
        // Text that is not well-formed UTF-8 is replaced when serialized, so it cannot be echoed exactly.
        const json state = std::string("bad\xff\xfe", 5);
        auto retry = input->retry_params(json::object(), {}, state, *p);
        EXPECT_TRUE(std::holds_alternative<std::string>(retry));
    }
}

TEST(ClientMechanismInput, AStateWithALoneSurrogateEscapeIsNeverSentBackAltered) {
    // The state as a server writes it: an escape of half a surrogate pair, which names no character.
    const json parsed = json::parse(R"({"requestState":"a\ud800b"})", nullptr, false);
    if (parsed.is_discarded()) {
        std::printf("lone surrogate escape: the parser rejects the text\n");
        return;
    }
    const json state = parsed.at("requestState");
    for (const auto& p : profiles_with_input(neomcp::InputDelivery::result)) {
        const auto input = client_input(*p);
        auto retry = input->retry_params(json::object(), {}, state, *p);
        if (const auto* problem = std::get_if<std::string>(&retry)) {
            std::printf("lone surrogate escape: kept by the parser, refused by the retry: %s\n", problem->c_str());
            continue;
        }
        // Sent: then what goes out is what was held, byte for byte.
        const json sent = json::parse(neomcp::serialize(std::get<json>(retry)), nullptr, false);
        ASSERT_FALSE(sent.is_discarded());
        EXPECT_EQ(sent.at("requestState"), state);
        std::printf("lone surrogate escape: the parser holds well-formed text, sent unchanged\n");
    }
}

TEST(ClientMechanismInput, ASamplingRequestWithToolBlocksIsRefusedAsMalformed) {
    const json params = J(R"({"messages":[{"role":"assistant","content":[{"type":"tool_use","id":"t1","name":"calc",
        "input":{}}]}],"maxTokens":10})");
    for (const auto& p : newest_first()) {
        const auto input = client_input(*p);
        if (!neomcp::builtin_mechanisms().make_input(p->mechanism(profile::Slot::server_input))->carries(*p, "sampling/createMessage")) {
            continue;
        }
        SCOPED_TRACE(p->revision());
        if (input->delivery() == neomcp::InputDelivery::requests) {
            json message = J(R"({"jsonrpc":"2.0","id":"s1","method":"sampling/createMessage"})");
            message["params"] = params;
            neomcp::Message request;
            ASSERT_NO_THROW(request = neomcp::classify(message));
            std::variant<neomcp::ServerAsk, neomcp::ServerPing, neomcp::RequestError> read{neomcp::ServerPing{}};
            ASSERT_NO_THROW(read = input->read_request(request, *p));
            ASSERT_TRUE(std::holds_alternative<neomcp::RequestError>(read));
            EXPECT_EQ(std::get<neomcp::RequestError>(read).code, neomcp::error_code::invalid_params);
        } else {
            json result = J(R"({"resultType":"input_required","inputRequests":{"s":{"method":"sampling/createMessage"}}})");
            result["inputRequests"]["s"]["params"] = params;
            EXPECT_TRUE(std::holds_alternative<std::string>(input->read_result(result, "tools/call", *p)));
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// server_input: the server sends requests

TEST(ClientMechanismInput, TheServersRequestsAreRecognisedAndAnswered) {
    const auto profiles = profiles_with_input(neomcp::InputDelivery::requests);
    ASSERT_FALSE(profiles.empty());
    for (const auto& p : profiles) {
        SCOPED_TRACE(p->revision());
        const auto input = client_input(*p);
        const auto self = self_for(*p);
        AskingHost host;
        Peer peer(host, session_options());
        open_connection(peer, p, self);
        peer.notify(frame_request(neomcp::RequestId(5), {"tools/call", J(R"({"name":"asks","arguments":{}})")}));
        // The host asks for every kind the client supports under the revision (roots and sampling in every revision
        // of this kind, a form where the client could declare forms); each comes as a request of its own.
        const std::size_t expected = self.capabilities.contains("elicitation") ? 3U : 2U;
        std::size_t answered = 0;
        while (answered < expected) {
            const auto asked = peer.next_request(patience);
            ASSERT_TRUE(asked.has_value()) << "request " << answered << " of " << expected;
            const auto request = neomcp::classify(*asked);
            auto read = input->read_request(request, *p);
            ASSERT_TRUE(std::holds_alternative<neomcp::ServerAsk>(read))
                << (std::holds_alternative<neomcp::RequestError>(read) ? std::get<neomcp::RequestError>(read).message : "");
            const auto& ask = std::get<neomcp::ServerAsk>(read);
            auto response = input->answer(*request.id, answer_for(ask.request), *p);
            ASSERT_TRUE(std::holds_alternative<json>(response)) << std::get<std::string>(response);
            peer.send(neomcp::serialize(std::get<json>(response)));
            ++answered;
        }
        const json done = result_of(peer.await_response(neomcp::RequestId(5), patience));
        EXPECT_FALSE(std::get<std::optional<neomcp::InputAsking>>(input->read_result(done, "tools/call", *p)).has_value());
        const auto last = host.last();
        ASSERT_TRUE(last.has_value());
        EXPECT_EQ(last->input_responses.size(), answered);

        // A ping is recognised and answered with an empty result.
        const auto ping = neomcp::classify(J(R"({"jsonrpc":"2.0","id":"p","method":"ping"})"));
        EXPECT_TRUE(std::holds_alternative<neomcp::ServerPing>(input->read_request(ping, *p)));
        EXPECT_EQ(input->ping_answer(neomcp::RequestId("p")), J(R"({"jsonrpc":"2.0","id":"p","result":{}})"));
        // An unknown method, malformed params and a message that is no request get errors to answer with.
        const auto unknown = input->read_request(neomcp::classify(J(R"({"jsonrpc":"2.0","id":1,"method":"x/y"})")), *p);
        ASSERT_TRUE(std::holds_alternative<neomcp::RequestError>(unknown));
        EXPECT_EQ(std::get<neomcp::RequestError>(unknown).code, neomcp::error_code::method_not_found);
        const auto malformed = input->read_request(
            neomcp::classify(J(R"({"jsonrpc":"2.0","id":1,"method":"sampling/createMessage","params":{"maxTokens":"x"}})")), *p);
        ASSERT_TRUE(std::holds_alternative<neomcp::RequestError>(malformed));
        EXPECT_EQ(std::get<neomcp::RequestError>(malformed).code, neomcp::error_code::invalid_params);
        const auto response = input->read_request(neomcp::classify(J(R"({"jsonrpc":"2.0","id":1,"result":{}})")), *p);
        ASSERT_TRUE(std::holds_alternative<neomcp::RequestError>(response));

        // What the revision prescribes for kinds the client did not declare.
        EXPECT_EQ(input->refusal(neomcp::RootsRequest{}, *p).code, neomcp::error_code::method_not_found);
        EXPECT_EQ(input->refusal(neomcp::SamplingRequest{}, *p).code, neomcp::error_code::method_not_found);
        EXPECT_EQ(input->refusal(neomcp::ElicitationUrl{}, *p).code, neomcp::error_code::invalid_params);
        // Retrying with answers does not apply.
        EXPECT_TRUE(std::holds_alternative<std::string>(input->retry_params(json::object(), {}, std::nullopt, *p)));
    }
}

TEST(ClientMechanismInput, RootsChangesAreToldOnlyWhereTheProfileHasTheNotification) {
    for (const auto& p : newest_first()) {
        const auto input = client_input(*p);
        const profile::Message* entry = p->notification("notifications/roots/list_changed");
        EXPECT_EQ(input->roots_changed(*p).has_value(), entry != nullptr && entry->from_client) << p->revision();
    }
}

// ---------------------------------------------------------------------------------------------------------------
// cancel

TEST(ClientMechanismCancel, TheCancellationIsReadByTheServerHalf) {
    for (const auto& p : newest_first()) {
        std::vector<std::string> names;
        if (p->stdio()) names.push_back(p->stdio()->cancel);
        if (p->http_sse()) names.push_back(p->http_sse()->cancel);
        if (p->streamable_http()) names.push_back(p->streamable_http()->cancel);
        for (const auto& name : names) {
            SCOPED_TRACE(p->revision() + " " + name);
            const auto client = client_registry().make_cancel(name);
            const auto server = neomcp::builtin_mechanisms().make_cancel(name);
            ASSERT_FALSE(client->check(*p).has_value());
            EXPECT_EQ(client->stream_close_cancels(), server->stops_on_stream_close());
            const auto notification = client->notification(neomcp::RequestId(42), "timed out", *p);
            if (client->stream_close_cancels()) {
                EXPECT_FALSE(notification.has_value());
                continue;
            }
            ASSERT_TRUE(notification.has_value());
            const auto read = server->cancelled_request(neomcp::classify(frame_notification(*notification)));
            ASSERT_TRUE(read.has_value());
            EXPECT_EQ(*read, neomcp::RequestId(42));
            // The server's cancellation of its own request is read the same way.
            const auto from_server = client->cancelled_request(
                neomcp::classify(J(R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":"r9"}})")));
            ASSERT_TRUE(from_server.has_value());
            EXPECT_EQ(*from_server, neomcp::RequestId("r9"));
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// change_feed

TEST(ClientMechanismFeed, WhatTheClientWritesIsReadBackByTheServerHalf) {
    for (const auto& p : newest_first()) {
        SCOPED_TRACE(p->revision());
        const auto client = client_feed(*p);
        const auto server = neomcp::builtin_mechanisms().make_feed(p->mechanism(profile::Slot::change_feed));
        ASSERT_EQ(client->delivery(), server->delivery());
        std::vector<neomcp::FeedRequest> asked;
        if (client->delivery() == neomcp::FeedDelivery::connection) {
            asked.push_back({neomcp::FeedRequest::Action::watch, "test://a", {}});
            asked.push_back({neomcp::FeedRequest::Action::unwatch, "test://a", {}});
            EXPECT_TRUE(std::holds_alternative<std::string>(
                client->write({neomcp::FeedRequest::Action::listen, "", {}}, *p)));
        } else {
            neomcp::FeedRequest listen;
            listen.action = neomcp::FeedRequest::Action::listen;
            listen.filter.tools_list = true;
            listen.filter.resources_list = true;
            listen.filter.resources = {"test://a", "test://b"};
            asked.push_back(listen);
            asked.push_back({neomcp::FeedRequest::Action::listen, "", {}});
            EXPECT_TRUE(std::holds_alternative<std::string>(
                client->write({neomcp::FeedRequest::Action::watch, "test://a", {}}, *p)));
        }
        for (const auto& one : asked) {
            auto written = client->write(one, *p);
            ASSERT_TRUE(std::holds_alternative<neomcp::ClientMessage>(written)) << std::get<std::string>(written);
            const auto& message = std::get<neomcp::ClientMessage>(written);
            ASSERT_TRUE(server->serves(message.method));
            auto read = server->read(neomcp::classify(frame_request(neomcp::RequestId(1), message)));
            ASSERT_TRUE(std::holds_alternative<neomcp::FeedRequest>(read)) << std::get<neomcp::RequestError>(read).message;
            const auto& back = std::get<neomcp::FeedRequest>(read);
            EXPECT_EQ(back.action, one.action);
            EXPECT_EQ(back.uri, one.uri);
            EXPECT_EQ(back.filter.tools_list, one.filter.tools_list);
            EXPECT_EQ(back.filter.prompts_list, one.filter.prompts_list);
            EXPECT_EQ(back.filter.resources_list, one.filter.resources_list);
            EXPECT_EQ(back.filter.resources, one.filter.resources);
        }
    }
}

// A server half's message, tagged for a subscription where the mechanism tags.
neomcp::Message server_notification(const neomcp::ChangeFeedMechanism& server, const neomcp::FeedMessage& message,
                                    const neomcp::RequestId& subscription) {
    json params = message.params;
    if (server.delivery() == neomcp::FeedDelivery::subscription) server.tag(params, subscription);
    return neomcp::classify(frame_notification({message.method, params}));
}

TEST(ClientMechanismFeed, TheServerHalfsMessagesAreRecognised) {
    const neomcp::Change changes[] = {{neomcp::ChangeKind::tools_list, ""},
                                      {neomcp::ChangeKind::prompts_list, ""},
                                      {neomcp::ChangeKind::resources_list, ""},
                                      {neomcp::ChangeKind::resource_updated, "test://a"}};
    for (const auto& p : newest_first()) {
        SCOPED_TRACE(p->revision());
        const auto client = client_feed(*p);
        const auto server = neomcp::builtin_mechanisms().make_feed(p->mechanism(profile::Slot::change_feed));
        const neomcp::RequestId subscription("sub-1");
        for (const auto& change : changes) {
            const auto message = server_notification(*server, server->notification(change), subscription);
            if (p->notification(message.method) == nullptr) continue;
            auto read = client->read(message, *p);
            ASSERT_TRUE(read.has_value()) << message.method;
            ASSERT_TRUE(std::holds_alternative<neomcp::FeedEvent>(*read)) << std::get<std::string>(*read);
            const auto& event = std::get<neomcp::FeedEvent>(*read);
            EXPECT_EQ(event.kind, neomcp::FeedEvent::Kind::change);
            EXPECT_EQ(event.change.kind, change.kind);
            EXPECT_EQ(event.change.uri, change.uri);
            if (client->delivery() == neomcp::FeedDelivery::subscription) {
                EXPECT_EQ(event.subscription, std::optional<neomcp::RequestId>(subscription));
            } else {
                EXPECT_FALSE(event.subscription.has_value());
            }
        }
        // A message that is no message of the feed.
        EXPECT_FALSE(client->read(neomcp::classify(J(R"({"jsonrpc":"2.0","method":"notifications/message","params":{}})")), *p)
                         .has_value());
        if (client->delivery() != neomcp::FeedDelivery::subscription) {
            EXPECT_TRUE(client->read_end(json::object(), subscription, *p).has_value());
            continue;
        }
        // The acknowledgement, the ending notice and the result that ends it.
        neomcp::ChangeFilter honoured;
        honoured.tools_list = true;
        honoured.resources = {"test://a"};
        auto ack = client->read(server_notification(*server, server->acknowledgement(honoured), subscription), *p);
        ASSERT_TRUE(ack.has_value());
        ASSERT_TRUE(std::holds_alternative<neomcp::FeedEvent>(*ack)) << std::get<std::string>(*ack);
        const auto& acknowledged = std::get<neomcp::FeedEvent>(*ack);
        EXPECT_EQ(acknowledged.kind, neomcp::FeedEvent::Kind::acknowledged);
        EXPECT_EQ(acknowledged.subscription, std::optional<neomcp::RequestId>(subscription));
        EXPECT_TRUE(acknowledged.honoured.tools_list);
        EXPECT_FALSE(acknowledged.honoured.prompts_list);
        EXPECT_EQ(acknowledged.honoured.resources, std::vector<std::string>{"test://a"});
        const auto notice = server->ending_notice(subscription);
        ASSERT_TRUE(notice.has_value());
        auto ended = client->read(neomcp::classify(frame_notification({notice->method, notice->params})), *p);
        ASSERT_TRUE(ended.has_value());
        ASSERT_TRUE(std::holds_alternative<neomcp::FeedEvent>(*ended));
        EXPECT_EQ(std::get<neomcp::FeedEvent>(*ended).kind, neomcp::FeedEvent::Kind::ended);
        EXPECT_EQ(std::get<neomcp::FeedEvent>(*ended).subscription, std::optional<neomcp::RequestId>(subscription));
        // A change without the subscription it belongs to, and a filter of the wrong kind, are malformed.
        auto untagged = client->read(neomcp::classify(J(R"({"jsonrpc":"2.0","method":"notifications/tools/list_changed"})")), *p);
        ASSERT_TRUE(untagged.has_value());
        EXPECT_TRUE(std::holds_alternative<std::string>(*untagged));
        auto bad_ack = client->read(neomcp::classify(J(R"({"jsonrpc":"2.0","method":"notifications/subscriptions/acknowledged",
            "params":{"notifications":{"toolsListChanged":"yes"},"_meta":{"io.modelcontextprotocol/subscriptionId":"s"}}})")), *p);
        ASSERT_TRUE(bad_ack.has_value());
        EXPECT_TRUE(std::holds_alternative<std::string>(*bad_ack));
    }
}

TEST(ClientMechanismFeed, WhatIsOfferedOnAConnectionFollowsTheServersCapabilities) {
    for (const auto& p : newest_first()) {
        const auto client = client_feed(*p);
        neomcp::ChangeFilter asked;
        asked.tools_list = true;
        asked.prompts_list = true;
        asked.resources_list = true;
        asked.resources = {"test://a"};
        const json capabilities = J(R"({"tools":{"listChanged":true},"prompts":{},"resources":{"subscribe":true}})");
        const auto offered = client->offered(asked, capabilities);
        if (client->delivery() == neomcp::FeedDelivery::subscription) {
            EXPECT_TRUE(offered.prompts_list);
            continue;
        }
        EXPECT_TRUE(offered.tools_list);
        EXPECT_FALSE(offered.prompts_list);
        EXPECT_FALSE(offered.resources_list);
        EXPECT_EQ(offered.resources, asked.resources);
        EXPECT_TRUE(client->offered(asked, J(R"({"resources":{"subscribe":false}})")).resources.empty());
    }
}

TEST(ClientMechanismFeed, ASessionsSubscriptionIsFollowedToItsEnd) {
    for (const auto& p : newest_first()) {
        const auto client = client_feed(*p);
        if (client->delivery() != neomcp::FeedDelivery::subscription) continue;
        SCOPED_TRACE(p->revision());
        const auto self = self_for(*p);
        AskingHost host;
        Peer peer(host, session_options());
        neomcp::FeedRequest listen;
        listen.action = neomcp::FeedRequest::Action::listen;
        listen.filter.tools_list = true;
        listen.filter.prompts_list = true;
        auto written = client->write(listen, *p);
        ASSERT_TRUE(std::holds_alternative<neomcp::ClientMessage>(written));
        const auto& message = std::get<neomcp::ClientMessage>(written);
        const neomcp::RequestId id("feed");
        peer.notify(client_request(*p, id, message.method, message.params, self));

        const auto first = peer.next_notification(patience);
        ASSERT_TRUE(first.has_value());
        auto ack = client->read(neomcp::classify(*first), *p);
        ASSERT_TRUE(ack.has_value());
        ASSERT_TRUE(std::holds_alternative<neomcp::FeedEvent>(*ack)) << std::get<std::string>(*ack);
        EXPECT_EQ(std::get<neomcp::FeedEvent>(*ack).kind, neomcp::FeedEvent::Kind::acknowledged);
        EXPECT_EQ(std::get<neomcp::FeedEvent>(*ack).subscription, std::optional<neomcp::RequestId>(id));
        EXPECT_TRUE(std::get<neomcp::FeedEvent>(*ack).honoured.tools_list);
        EXPECT_FALSE(std::get<neomcp::FeedEvent>(*ack).honoured.prompts_list);

        ASSERT_GT(host.report({neomcp::ChangeKind::tools_list, ""}), 0);
        const auto change = peer.next_notification(patience);
        ASSERT_TRUE(change.has_value());
        auto read = client->read(neomcp::classify(*change), *p);
        ASSERT_TRUE(read.has_value());
        ASSERT_TRUE(std::holds_alternative<neomcp::FeedEvent>(*read)) << std::get<std::string>(*read);
        EXPECT_EQ(std::get<neomcp::FeedEvent>(*read).change.kind, neomcp::ChangeKind::tools_list);
        EXPECT_EQ(std::get<neomcp::FeedEvent>(*read).subscription, std::optional<neomcp::RequestId>(id));

        // Closing the session ends the subscription gracefully: the notice, then the result.
        peer.close();
        bool saw_notice = false;
        bool saw_end = false;
        for (const json& line : peer.transcript()) {
            const auto one = neomcp::classify(line);
            if (one.kind == neomcp::MessageKind::notification) {
                auto event = client->read(one, *p);
                if (event && std::holds_alternative<neomcp::FeedEvent>(*event) &&
                    std::get<neomcp::FeedEvent>(*event).kind == neomcp::FeedEvent::Kind::ended) {
                    EXPECT_EQ(std::get<neomcp::FeedEvent>(*event).subscription, std::optional<neomcp::RequestId>(id));
                    saw_notice = true;
                }
            } else if (one.kind == neomcp::MessageKind::response && one.id == std::optional<neomcp::RequestId>(id)) {
                ASSERT_FALSE(one.failed) << line.dump();
                EXPECT_FALSE(client->read_end(one.outcome, id, *p).has_value()) << line.dump();
                EXPECT_TRUE(client->read_end(one.outcome, neomcp::RequestId("other"), *p).has_value());
                // An ending without the subscription's tag, or with a `_meta` of the wrong kind, is malformed.
                json untagged = one.outcome;
                untagged["_meta"] = json::object();
                EXPECT_TRUE(client->read_end(untagged, id, *p).has_value());
                json bad_meta = one.outcome;
                bad_meta["_meta"] = 5;
                EXPECT_TRUE(client->read_end(bad_meta, id, *p).has_value());
                saw_end = true;
            }
        }
        EXPECT_TRUE(saw_notice);
        EXPECT_TRUE(saw_end);
    }
}

TEST(ClientMechanismFeed, AConnectionsNotificationsAreRecognised) {
    for (const auto& p : handshake_candidates()) {
        const auto client = client_feed(*p);
        if (client->delivery() != neomcp::FeedDelivery::connection) continue;
        SCOPED_TRACE(p->revision());
        AskingHost host;
        Peer peer(host, session_options());
        const auto accepted = open_connection(peer, p, self_for(*p));
        neomcp::ChangeFilter asked;
        asked.tools_list = true;
        EXPECT_TRUE(client->offered(asked, accepted.server.capabilities).tools_list);
        ASSERT_GT(host.report({neomcp::ChangeKind::tools_list, ""}), 0);
        const auto change = peer.next_notification(patience);
        ASSERT_TRUE(change.has_value());
        auto read = client->read(neomcp::classify(*change), *p);
        ASSERT_TRUE(read.has_value());
        ASSERT_TRUE(std::holds_alternative<neomcp::FeedEvent>(*read)) << std::get<std::string>(*read);
        EXPECT_EQ(std::get<neomcp::FeedEvent>(*read).change.kind, neomcp::ChangeKind::tools_list);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// batch

TEST(ClientMechanismBatch, ArraysAreSplitOnlyWhereTheRevisionHasThem) {
    for (const auto name : profile::declared_mechanisms(profile::Slot::batch)) {
        const auto batch = client_registry().make_batch(name);
        const auto server = neomcp::builtin_mechanisms().make_batch(name);
        EXPECT_EQ(batch->reads_arrays(), server->accepts_arrays()) << name;
        auto single = batch->split(J(R"({"jsonrpc":"2.0","method":"x"})"));
        ASSERT_TRUE(std::holds_alternative<std::vector<json>>(single));
        EXPECT_EQ(std::get<std::vector<json>>(single).size(), 1U);
        auto two = batch->split(J(R"([{"jsonrpc":"2.0","method":"x"},{"jsonrpc":"2.0","method":"y"}])"));
        if (batch->reads_arrays()) {
            ASSERT_TRUE(std::holds_alternative<std::vector<json>>(two));
            EXPECT_EQ(std::get<std::vector<json>>(two).size(), 2U);
        } else {
            EXPECT_TRUE(std::holds_alternative<std::string>(two));
        }
        EXPECT_TRUE(std::holds_alternative<std::string>(batch->split(J("[]"))));
    }
}

// ---------------------------------------------------------------------------------------------------------------
// malformed messages

std::vector<json> odd_values() {
    std::vector<json> values;
    for (const char* text :
         {"null", "true", "0", "-1", "1.5", "\"text\"", "[]", "[1]", "[{}]", "{}", R"({"_meta":5})", R"({"_meta":[]})",
          R"({"resultType":null})", R"({"resultType":"input_required","inputRequests":{"a":{"method":5}}})",
          R"({"resultType":"input_required","inputRequests":{"a":{"method":"elicitation/create","params":{"mode":7}}}})",
          R"({"resultType":"input_required","inputRequests":{"a":{"method":"sampling/createMessage","params":{"messages":5}}}})",
          R"({"protocolVersion":"2025-11-25","capabilities":[],"serverInfo":5,"instructions":3})",
          R"({"supportedVersions":[1],"capabilities":5,"cacheScope":1,"resultType":1,"ttlMs":"x"})",
          R"({"code":"x","data":{"supported":"x"}})", R"({"code":-32022,"data":5})", R"({"code":-32602,"data":{"supported":{}}})",
          R"({"uri":5,"notifications":5,"requestId":{},"_meta":{"io.modelcontextprotocol/subscriptionId":[]}})",
          R"({"notifications":{"resourceSubscriptions":[1]},"_meta":{"io.modelcontextprotocol/subscriptionId":"s"}})",
          R"({"messages":[{"role":"x","content":5}],"maxTokens":-1})", R"({"message":5,"requestedSchema":"x"})"}) {
        values.push_back(J(text));
    }
    return values;
}

TEST(ClientMechanismMalformed, NoReaderThrowsForWhatAServerSends) {
    const auto values = odd_values();
    const auto candidates = handshake_candidates();
    const char* server_methods[] = {"ping", "elicitation/create", "sampling/createMessage", "roots/list", "x/y"};
    const char* notifications[] = {"notifications/tools/list_changed", "notifications/resources/updated",
                                   "notifications/subscriptions/acknowledged", "notifications/cancelled"};
    for (const auto& p : newest_first()) {
        SCOPED_TRACE(p->revision());
        const auto open = client_open(*p);
        const auto input = client_input(*p);
        const auto feed = client_feed(*p);
        const auto cancel = client_registry().make_cancel(p->stdio()->cancel);
        const auto batch = client_registry().make_batch(p->mechanism(profile::Slot::batch));
        for (const json& value : values) {
            SCOPED_TRACE(value.dump());
            EXPECT_NO_THROW(static_cast<void>(open->read_handshake(value, candidates)));
            EXPECT_NO_THROW(static_cast<void>(open->read_discovery(value, *p)));
            EXPECT_NO_THROW(static_cast<void>(open->read_version_refusal(value, *p)));
            EXPECT_NO_THROW(static_cast<void>(open->level_request(neomcp::LogLevel::info, *p, value)));
            EXPECT_NO_THROW(static_cast<void>(input->read_result(value, "tools/call", *p)));
            EXPECT_NO_THROW(static_cast<void>(feed->read_end(value, neomcp::RequestId(1), *p)));
            EXPECT_NO_THROW(static_cast<void>(feed->offered(neomcp::ChangeFilter{}, value)));
            EXPECT_NO_THROW(static_cast<void>(batch->split(value)));
            for (const char* method : server_methods) {
                json message = J(R"({"jsonrpc":"2.0","id":3})");
                message["method"] = method;
                if (value.is_object() || value.is_array()) message["params"] = value;
                const auto classified = neomcp::classify(message);
                EXPECT_NO_THROW(static_cast<void>(input->read_request(classified, *p))) << method;
            }
            for (const char* method : notifications) {
                json message = J(R"({"jsonrpc":"2.0"})");
                message["method"] = method;
                if (value.is_object() || value.is_array()) message["params"] = value;
                const auto classified = neomcp::classify(message);
                EXPECT_NO_THROW(static_cast<void>(feed->read(classified, *p))) << method;
                EXPECT_NO_THROW(static_cast<void>(cancel->cancelled_request(classified))) << method;
            }
            // A value that asks for input must be read as complete or as an error, never as a request it is not.
            auto read = input->read_result(value, "tools/call", *p);
            if (const auto* asking = std::get_if<std::optional<neomcp::InputAsking>>(&read)) {
                EXPECT_FALSE(asking->has_value());
            }
        }
    }
}

}  // namespace
