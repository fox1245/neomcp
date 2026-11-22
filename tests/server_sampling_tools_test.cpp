// Sampling with tools as the server writes it on the wire: the two rounds of a tool (tools offered, the model's tool
// use, then the history with the tool's result) under the revisions whose sampling carries tool blocks, in both ways
// a server asks; and under the others, and towards a client that did not declare tools, that the call fails at once and
// no request is sent.
//
// The revisions are chosen from the profiles' data (the era, whether a server asks for sampling, whether the union of
// sampling blocks lists the tool blocks), never by name.
#include <algorithm>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/jsonrpc.h"
#include "neomcp/profile.h"
#include "neomcp/server.h"
#include "neomcp/testing/peer.h"
#include "support/sampling_tool_loop.h"

namespace {

using namespace std::chrono_literals;
using neomcp::json;
using neomcp::testing::Peer;
namespace profile = neomcp::profile;
namespace loop = sampling_tool_loop;

constexpr std::chrono::milliseconds patience = 30s;
constexpr std::string_view sampling_method = "sampling/createMessage";

json J(const char* text) { return json::parse(text); }

bool carries_tool_blocks(const profile::Profile& profile) {
    const auto* blocks = profile.union_type("SamplingMessageContentBlock");
    if (blocks == nullptr) return false;
    for (const char* type : {"ToolUseContent", "ToolResultContent"}) {
        if (profile.type(type) == nullptr ||
            std::find(blocks->alternatives.begin(), blocks->alternatives.end(), type) == blocks->alternatives.end()) {
            return false;
        }
    }
    return true;
}

// The newest built-in revision of the era that sends sampling requests of its own (or asks inside a result, for the
// modern era) and whose sampling carries tool blocks or not.
profile::ProfilePtr revision_where(profile::Era era, bool with_tools) {
    profile::ProfilePtr found;
    for (const auto revision : profile::builtin_revisions()) {
        auto candidate = profile::builtin(revision);
        const auto* request = candidate->request(sampling_method);
        const bool asks = (request != nullptr && request->from_server) || candidate->input_request(sampling_method) != nullptr;
        if (candidate->era() == era && asks && carries_tool_blocks(*candidate) == with_tools) found = std::move(candidate);
    }
    return found;
}

neomcp::ServerOptions options_for(const profile::ProfilePtr& revision) {
    neomcp::ServerOptions options;
    options.name = "sampling-tools";
    options.version = "1.0.0";
    options.request_state_keys = {std::string(32, 's')};
    options.profiles = {revision};
    return options;
}

json tools_call(int id) {
    json message = json::object();
    message["jsonrpc"] = "2.0";
    message["id"] = id;
    message["method"] = "tools/call";
    message["params"] = json{{"name", std::string(loop::tool_name)}, {"arguments", json::object()}};
    return message;
}

// A client of a revision with a handshake, with the capabilities given.
struct HandshakeClient {
    HandshakeClient(neomcp::Server& server, profile::ProfilePtr revision, json capabilities)
        : peer(server), profile(std::move(revision)) {
        const auto opened =
            peer.request(Peer::initialize_request(*profile, neomcp::RequestId("init"), std::move(capabilities)), patience);
        EXPECT_TRUE(opened && opened->contains("result"));
        peer.notify(J(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
    }
    void answer(const json& request, json result) {
        json response = json::object();
        response["jsonrpc"] = "2.0";
        response["id"] = request.at("id");
        response["result"] = std::move(result);
        peer.send(neomcp::serialize(response));
    }
    // Whether the session sent the client a sampling request at all.
    bool sent_sampling() const {
        const auto lines = peer.transcript();
        return std::any_of(lines.begin(), lines.end(),
                           [](const json& line) { return line.contains("method") && line.at("method") == sampling_method; });
    }

    Peer peer;
    profile::ProfilePtr profile;
};

json tool_use_answer() {
    return json{{"role", "assistant"},
                {"content", json{{"type", "tool_use"}, {"id", std::string(loop::use_id)}, {"name", "add"}, {"input", {{"a", 2}, {"b", 3}}}}},
                {"model", "m"},
                {"stopReason", "toolUse"}};
}

json text_answer(const std::string& text) {
    return json{{"role", "assistant"}, {"content", json{{"type", "text"}, {"text", text}}}, {"model", "m"}};
}

std::string text_of(const json& answer) { return answer.at("result").at("content").at(0).at("text").get<std::string>(); }

// What the second round must hold: the question, the model's tool use as it was, and the tool's result for its id.
void check_second_round(const json& params) {
    const json& messages = params.at("messages");
    ASSERT_EQ(messages.size(), 3U) << params.dump();
    EXPECT_EQ(messages.at(1).at("role"), "assistant");
    const json& use = messages.at(1).at("content");
    EXPECT_EQ(use.at("type"), "tool_use");
    EXPECT_EQ(use.at("id"), loop::use_id);
    EXPECT_EQ(use.at("input"), (json{{"a", 2}, {"b", 3}}));
    EXPECT_EQ(messages.at(2).at("role"), "user");
    const json& result = messages.at(2).at("content");
    EXPECT_EQ(result.at("type"), "tool_result");
    EXPECT_EQ(result.at("toolUseId"), loop::use_id);
    EXPECT_EQ(result.at("content"), json::array({json{{"type", "text"}, {"text", "5"}}}));
    EXPECT_EQ(params.at("toolChoice"), (json{{"mode", "none"}}));
}

TEST(SamplingToolsWire, TwoRoundsThroughRequestsOfTheSession) {
    const auto revision = revision_where(profile::Era::legacy, true);
    ASSERT_TRUE(revision) << "a revision with a handshake whose sampling carries tool blocks";
    SCOPED_TRACE(revision->revision());
    neomcp::Server server(options_for(revision));
    loop::register_tool(server);
    HandshakeClient client(server, revision, J(R"({"sampling":{"tools":{}}})"));
    client.peer.send(neomcp::serialize(tools_call(7)));

    const auto first = client.peer.next_request(patience);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->at("method"), sampling_method);
    EXPECT_EQ(first->at("params").at("tools").at(0).at("name"), loop::tool_offered);
    client.answer(*first, tool_use_answer());

    const auto second = client.peer.next_request(patience);
    ASSERT_TRUE(second);
    check_second_round(second->at("params"));
    const std::string said = "the tool " + std::string(loop::use_id) + " gave 5";
    client.answer(*second, text_answer(said));

    const auto done = client.peer.await_response(*neomcp::RequestId::from_json(json(7)), patience);
    ASSERT_TRUE(done);
    ASSERT_TRUE(done->contains("result")) << done->dump();
    EXPECT_EQ(text_of(*done), said);
}

TEST(SamplingToolsWire, TwoRoundsThroughResultsThatAsk) {
    const auto revision = revision_where(profile::Era::modern, true);
    ASSERT_TRUE(revision) << "a revision without a handshake whose sampling carries tool blocks";
    SCOPED_TRACE(revision->revision());
    neomcp::Server server(options_for(revision));
    loop::register_tool(server);
    Peer peer(server);
    const json capabilities = J(R"({"sampling":{"tools":{}}})");
    int next_id = 1;
    const auto call = [&](json params) {
        params["name"] = std::string(loop::tool_name);
        params["arguments"] = json::object();
        params["_meta"] = Peer::request_meta(*revision, capabilities);
        json message{{"jsonrpc", "2.0"}, {"id", next_id++}, {"method", "tools/call"}, {"params", std::move(params)}};
        const auto answer = peer.request(message, patience);
        if (!answer) throw std::runtime_error("no answer within the patience");
        return *answer;
    };
    const auto retry = [&](const json& earlier, const char* key, json answer) {
        json params = json::object();
        params["inputResponses"] = json{{key, std::move(answer)}};
        if (earlier.at("result").contains("requestState")) params["requestState"] = earlier.at("result").at("requestState");
        return call(std::move(params));
    };

    const json first = call(json::object());
    ASSERT_TRUE(first.contains("result")) << first.dump();
    const json& asked = first.at("result").at("inputRequests").at("use");
    EXPECT_EQ(asked.at("method"), sampling_method);
    EXPECT_EQ(asked.at("params").at("tools").at(0).at("name"), loop::tool_offered);

    const json second = retry(first, "use", tool_use_answer());
    ASSERT_TRUE(second.contains("result")) << second.dump();
    const json& again = second.at("result").at("inputRequests").at("final");
    check_second_round(again.at("params"));

    const std::string said = "the tool " + std::string(loop::use_id) + " gave 5";
    const json third = retry(second, "final", text_answer(said));
    ASSERT_TRUE(third.contains("result")) << third.dump();
    EXPECT_EQ(text_of(third), said);
}

// A revision whose sampling has no tool blocks: the call fails at once, and no sampling request is sent. A client that
// cannot declare tools under that revision is told the capability the call needs, as any call that needs one; one that
// declares them anyway gets the error of a request the revision cannot carry.
TEST(SamplingToolsWire, ARevisionWithoutToolBlocksSendsNothing) {
    const auto revision = revision_where(profile::Era::legacy, false);
    ASSERT_TRUE(revision) << "a revision whose sampling carries no tool blocks";
    SCOPED_TRACE(revision->revision());
    {
        neomcp::Server server(options_for(revision));
        loop::register_tool(server);
        HandshakeClient client(server, revision, J(R"({"sampling":{}})"));
        const auto done = client.peer.request(tools_call(3), patience);
        ASSERT_TRUE(done);
        EXPECT_FALSE(client.sent_sampling());
        ASSERT_TRUE(done->contains("result")) << done->dump();
        EXPECT_TRUE(done->at("result").value("isError", false)) << done->dump();
        EXPECT_NE(text_of(*done).find(R"("tools")"), std::string::npos) << done->dump();
    }
    {
        neomcp::Server server(options_for(revision));
        loop::register_tool(server);
        HandshakeClient client(server, revision, J(R"({"sampling":{"tools":{}}})"));
        const auto done = client.peer.request(tools_call(3), patience);
        ASSERT_TRUE(done);
        EXPECT_FALSE(client.sent_sampling());
        ASSERT_TRUE(done->contains("error")) << done->dump();
        EXPECT_EQ(done->at("error").at("code"), neomcp::error_code::internal_error) << done->dump();
    }
}

// A client that did not declare tools is never sent a request with tools: the call fails with the capability it lacks.
TEST(SamplingToolsWire, AClientWithoutTheToolsCapabilityIsNotAsked) {
    const auto revision = revision_where(profile::Era::legacy, true);
    ASSERT_TRUE(revision);
    SCOPED_TRACE(revision->revision());
    neomcp::Server server(options_for(revision));
    loop::register_tool(server);
    HandshakeClient client(server, revision, J(R"({"sampling":{}})"));
    const auto done = client.peer.request(tools_call(4), patience);
    ASSERT_TRUE(done);
    EXPECT_FALSE(client.sent_sampling());
    ASSERT_TRUE(done->contains("result")) << done->dump();
    EXPECT_TRUE(done->at("result").value("isError", false)) << done->dump();
    EXPECT_NE(text_of(*done).find(R"("tools")"), std::string::npos) << "the call names what the client lacks";
}

}  // namespace
