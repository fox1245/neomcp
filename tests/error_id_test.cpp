// The `id` of an error answer to a message whose id could not be read, in every revision.
//
// The schemas of the revisions disagree with each other and with JSON-RPC: 2024-11-05, 2025-03-26 and 2025-06-18
// require `id` in an error response and allow a string or an integer there; 2025-11-25 and 2026-07-28 make `id`
// optional and allow a string or an integer. So `null` is allowed by none of them. Where `id` is optional the answer
// leaves it out; where it is required no answer can conform, and the answer carries the `null` that JSON-RPC
// prescribes.
#include <algorithm>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/profile.h"
#include "neomcp/session.h"
#include "neomcp/testing/peer.h"
#include "neomcp/tool_host.h"

namespace {

using neomcp::json;
namespace profile = neomcp::profile;

// What the pages of each revision say, written from the schemas, not read from the profiles.
const std::vector<std::pair<std::string, bool>> revisions{
    {"2024-11-05", false}, {"2025-03-26", false}, {"2025-06-18", false}, {"2025-11-25", true}, {"2026-07-28", true}};

class EmptyHost : public neomcp::ToolHost {
public:
    std::span<const neomcp::ToolDefinition> tools() const override { return {}; }
    void call(const neomcp::CallRequest&, neomcp::CallControl, neomcp::Completion) override {}
};

class Rig {
public:
    Rig(profile::Transport transport, std::vector<profile::ProfilePtr> served) {
        neomcp::SessionOptions options;
        options.transport = transport;
        options.profiles = std::move(served);
        auto made = neomcp::Session::create_tagged(options, host_, [this](neomcp::Output output) {
            const std::lock_guard<std::mutex> lock(mutex_);
            outputs_.push_back(std::move(output));
        });
        if (const auto* error = std::get_if<neomcp::CreateError>(&made)) throw std::runtime_error(error->message);
        session = std::move(std::get<std::unique_ptr<neomcp::Session>>(made));
    }

    // Feeds a line as a unit and returns the one answer it got.
    json answer_to(const std::string& line, const std::vector<std::pair<std::string, std::string>>& fields = {}) {
        std::vector<neomcp::HeaderField> headers;
        for (const auto& [name, value] : fields) headers.push_back(neomcp::HeaderField{name, value});
        session->receive(++unit_, line, headers);
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& output : outputs_) {
            if (output.unit == unit_ && output.kind == neomcp::OutputKind::response) return json::parse(output.line);
        }
        throw std::runtime_error("no answer");
    }

    std::unique_ptr<neomcp::Session> session;

private:
    EmptyHost host_;
    std::mutex mutex_;
    std::vector<neomcp::Output> outputs_;
    neomcp::UnitId unit_ = 0;
};

// What cannot be read: not JSON, and a request whose id is not a string or an integer.
std::vector<std::pair<std::string, std::string>> unreadable() {
    return {{"not JSON", "{"},
            {"an id that is an object", R"({"jsonrpc":"2.0","id":{},"method":"ping"})"},
            {"an id that is an array", R"({"jsonrpc":"2.0","id":[1],"method":"ping"})"},
            {"a message without a method or a result", R"({"jsonrpc":"2.0","id":{}})"}};
}

// The error response types of the revision's wire tables: those that have the members `error` and `id`.
std::vector<profile::Type> error_types(const profile::Profile& wire) {
    std::vector<profile::Type> found;
    for (const auto& type : wire.types()) {
        if (type.has_property("error") && type.has_property("id")) found.push_back(type);
    }
    return found;
}

// Whether the answer is an instance of the error response type: every member it has is a property of the type, every
// required property is a member, and `id`, when it is there, is a string or an integer.
bool conforms(const json& answer, const profile::Type& type) {
    for (const auto& member : answer.items()) {
        if (!type.has_property(member.key())) return false;
    }
    for (const auto& required : type.required) {
        if (!answer.contains(required)) return false;
    }
    if (answer.contains("id") && !answer.at("id").is_string() && !answer.at("id").is_number_integer()) return false;
    return true;
}

TEST(ErrorId, TheProfilesSayWhetherAnErrorResponseMayLeaveOutItsId) {
    for (const auto& [revision, may_omit] : revisions) {
        SCOPED_TRACE(revision);
        const auto wire = profile::builtin(revision);
        ASSERT_TRUE(wire);
        EXPECT_EQ(wire->error_response_may_omit_id(), may_omit);
        EXPECT_FALSE(error_types(*wire).empty());
    }
}

TEST(ErrorId, ARevisionWithoutTheTypesCannotOmitIt) {
    json document = json::parse(profile::builtin_text("2025-11-25"));
    document["wire"]["types"].erase("JSONRPCErrorResponse");
    document["wire"]["types"].erase("URLElicitationRequiredError");
    // The unions name types; take them out as well.
    for (const char* name : {"JSONRPCMessage", "JSONRPCResponse"}) {
        auto& alternatives = document["wire"]["unions"][name];
        json kept = json::array();
        for (const json& alternative : alternatives) {
            if (alternative != "JSONRPCErrorResponse") kept.push_back(alternative);
        }
        alternatives = kept;
    }
    const auto loaded = profile::load(document.dump());
    if (const auto* problem = std::get_if<profile::Error>(&loaded)) {
        GTEST_SKIP() << "the document without the types does not load: " << problem->pointer << " " << problem->message;
    }
    EXPECT_FALSE(std::get<profile::ProfilePtr>(loaded)->error_response_may_omit_id());
}

TEST(ErrorId, AnAnswerToWhatCannotBeReadConformsToTheRevisionAsFarAsItCan) {
    for (const auto& [revision, may_omit] : revisions) {
        const auto wire = profile::builtin(revision);
        for (const auto transport : {profile::Transport::stdio, profile::Transport::streamable_http}) {
            if (transport == profile::Transport::streamable_http && !wire->streamable_http()) continue;
            for (const auto& [what, line] : unreadable()) {
                SCOPED_TRACE(revision + " " + std::string(profile::to_string(transport)) + " " + what);
                Rig rig(transport, {wire});
                const json answer = rig.answer_to(line);
                ASSERT_TRUE(answer.contains("error")) << answer.dump();
                if (may_omit) {
                    EXPECT_FALSE(answer.contains("id")) << answer.dump();
                    bool one = false;
                    for (const auto& type : error_types(*wire)) one = one || conforms(answer, type);
                    EXPECT_TRUE(one) << "no error response type of the revision accepts " << answer.dump();
                } else {
                    // Every member is a property, and the only thing the schema cannot take is `id` null.
                    ASSERT_TRUE(answer.contains("id")) << answer.dump();
                    EXPECT_TRUE(answer.at("id").is_null()) << answer.dump();
                    json without_id = answer;
                    without_id.erase("id");
                    for (const auto& type : error_types(*wire)) {
                        for (const auto& member : without_id.items()) EXPECT_TRUE(type.has_property(member.key()));
                        EXPECT_TRUE(type.is_required("id")) << "a revision that requires `id` has no form without it";
                    }
                }
            }
        }
    }
}

TEST(ErrorId, ARequestWhoseIdWasReadIsAnsweredWithIt) {
    for (const auto& [revision, may_omit] : revisions) {
        SCOPED_TRACE(revision);
        const auto wire = profile::builtin(revision);
        Rig rig(profile::Transport::stdio, {wire});
        const json answer = rig.answer_to(R"({"jsonrpc":"2.0","id":7,"method":"no/such/method"})");
        ASSERT_TRUE(answer.contains("error")) << answer.dump();
        EXPECT_EQ(answer.at("id"), 7);
    }
}

TEST(ErrorId, ASessionOfAllTheRevisionsLeavesTheIdOutOnlyOnceTheClientsRevisionIsKnown) {
    std::vector<profile::ProfilePtr> all;
    for (const auto& item : revisions) all.push_back(profile::builtin(item.first));
    // Nothing tells the revision: one of those served requires the member, so it stays, as null.
    {
        Rig rig(profile::Transport::stdio, all);
        const json answer = rig.answer_to("{");
        ASSERT_TRUE(answer.contains("id")) << answer.dump();
        EXPECT_TRUE(answer.at("id").is_null());
    }
    // The connection a client opened tells it.
    for (const auto& [revision, may_omit] : revisions) {
        SCOPED_TRACE(revision);
        const auto wire = profile::builtin(revision);
        if (wire->request("initialize") == nullptr) continue;  // a revision that opens no connection this way
        Rig rig(profile::Transport::stdio, all);
        const json opened = rig.answer_to(neomcp::testing::Peer::initialize_request(*wire, 1).dump());
        ASSERT_TRUE(opened.contains("result")) << opened.dump();
        const json answer = rig.answer_to("{");
        EXPECT_EQ(!answer.contains("id"), may_omit) << answer.dump();
    }
    // A version header tells it, over the transport that has one.
    {
        Rig rig(profile::Transport::streamable_http, all);
        const json answer = rig.answer_to("{", {{"MCP-Protocol-Version", "2026-07-28"}});
        EXPECT_FALSE(answer.contains("id")) << answer.dump();
    }
    {
        Rig rig(profile::Transport::streamable_http, all);
        const json answer = rig.answer_to("{", {{"MCP-Protocol-Version", "2025-03-26"}});
        ASSERT_TRUE(answer.contains("id")) << answer.dump();
        EXPECT_TRUE(answer.at("id").is_null());
    }
}

}  // namespace
