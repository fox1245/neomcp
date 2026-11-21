// Errors of the application's own at the host interface: a host that completes a call with an ApplicationFailure, served
// by a session of every revision. The answer carries the code, message and data the host gave when the revision lets an
// application use the code, and is an internal error with a fixed message when it does not or when it is too long.
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/jsonrpc.h"
#include "neomcp/session.h"
#include "neomcp/testing/peer.h"
#include "neomcp/tool_host.h"

namespace {

using neomcp::json;
using neomcp::testing::Peer;

constexpr std::chrono::milliseconds patience = std::chrono::seconds(30);
constexpr std::string_view secret_message = "the application's own words";
constexpr std::string_view secret_data = "the application's own data";
constexpr std::string_view fixed_message = "Internal error: the tool call failed";

// A host with one tool, "fail", which completes with the failure that was set.
class FailingHost final : public neomcp::ToolHost {
public:
    FailingHost() {
        neomcp::ToolDefinition tool;
        tool.name = "fail";
        tool.input_schema = json{{"type", "object"}};
        tools_.push_back(std::move(tool));
        neomcp::ToolDefinition fine;
        fine.name = "fine";
        fine.input_schema = json{{"type", "object"}};
        tools_.push_back(std::move(fine));
    }
    std::span<const neomcp::ToolDefinition> tools() const override { return tools_; }
    void call(const neomcp::CallRequest& request, neomcp::CallControl, neomcp::Completion done) override {
        if (request.name == "fine") {
            done(neomcp::CallOutcome(neomcp::ToolResult{}));
            return;
        }
        const std::lock_guard<std::mutex> lock(mutex);
        done(neomcp::CallOutcome(failure));
    }

    std::mutex mutex;
    neomcp::ApplicationFailure failure;

private:
    std::vector<neomcp::ToolDefinition> tools_;
};

std::vector<std::string> revisions() {
    std::vector<std::string> found;
    for (const std::string_view revision : neomcp::profile::builtin_revisions()) {
        if (neomcp::profile::builtin(revision)->stdio()) found.emplace_back(revision);
    }
    return found;
}

// One session of the revision over `host`, opened the way the revision opens one.
struct Rig {
    Rig(FailingHost& host, const std::string& revision, bool expose = false, std::size_t max_message_bytes = 0)
        : profile(neomcp::profile::builtin(revision)) {
        neomcp::SessionOptions options;
        options.profiles.push_back(profile);
        options.expose_error_details = expose;
        if (max_message_bytes != 0) options.max_message_bytes = max_message_bytes;
        peer = std::make_unique<Peer>(host, std::move(options));
        if (profile->era() == neomcp::profile::Era::legacy) {
            const auto opened = peer->initialize(*profile, patience);
            EXPECT_TRUE(opened && opened->contains("result")) << (opened ? opened->dump() : "none");
        }
    }

    json request(int id, const std::string& tool = "fail") const {
        return Peer::make_request(*profile, id, "tools/call", json{{"name", tool}, {"arguments", json::object()}});
    }

    json answer(int id, const std::string& tool = "fail") {
        auto got = peer->request(request(id, tool), patience);
        EXPECT_TRUE(got.has_value());
        return got ? *got : json();
    }

    neomcp::profile::ProfilePtr profile;
    std::unique_ptr<Peer> peer;
};

neomcp::ApplicationFailure failure_of(int code) {
    return neomcp::ApplicationFailure{code, std::string(secret_message), json{{"note", std::string(secret_data)}}};
}

// The answer is the internal error with the fixed message and nothing of the application's.
void expect_internal_error(const json& answer, const std::string& where) {
    ASSERT_TRUE(answer.contains("error")) << where << ": " << answer.dump();
    EXPECT_EQ(answer["error"]["code"], neomcp::error_code::internal_error) << where;
    EXPECT_EQ(answer["error"]["message"], std::string(fixed_message)) << where;
    EXPECT_FALSE(answer["error"].contains("data")) << where;
    const std::string line = answer.dump();
    EXPECT_EQ(line.find(secret_message), std::string::npos) << where;
    EXPECT_EQ(line.find(secret_data), std::string::npos) << where;
}

// What a revision names, from its profile.
std::set<int> named_codes(const neomcp::profile::Profile& profile) {
    std::set<int> codes;
    for (const auto& named : profile.error_codes()) codes.insert(named.code);
    for (const int code : profile.not_emitted()) codes.insert(code);
    return codes;
}

TEST(ApplicationErrorsHost, AnAllowedCodeIsAnsweredWithItsCodeMessageAndDataUnderEveryRevision) {
    const std::vector<int> codes = {1,      -1,     1000000, -31999, -32099, -32050,
                                    -32001, -32000, -33000,  -32769, -2147483647 - 1, 2147483647};
    for (const std::string& revision : revisions()) {
        FailingHost host;
        Rig rig(host, revision);
        const auto named = named_codes(*rig.profile);
        int id = 1;
        for (const int code : codes) {
            if (named.contains(code)) continue;
            host.failure = failure_of(code);
            const json answer = rig.answer(++id);
            const std::string where = revision + " code " + std::to_string(code);
            ASSERT_TRUE(answer.contains("error")) << where << ": " << answer.dump();
            EXPECT_EQ(answer["error"]["code"], code) << where;
            EXPECT_EQ(answer["error"]["message"], std::string(secret_message)) << where;
            EXPECT_EQ(answer["error"]["data"], json({{"note", std::string(secret_data)}})) << where;
            EXPECT_EQ(answer["id"], id) << where;
        }
        // No data member when there are none.
        host.failure = neomcp::ApplicationFailure{-32050, "plain", nullptr};
        const json plain = rig.answer(900);
        EXPECT_EQ(plain["error"]["code"], -32050) << revision;
        EXPECT_FALSE(plain["error"].contains("data")) << revision << ": " << plain.dump();
        // The session goes on serving.
        EXPECT_TRUE(rig.answer(901, "fine").contains("result")) << revision;
    }
}

TEST(ApplicationErrorsHost, AnyKindOfDataIsSentAsGiven) {
    FailingHost host;
    Rig rig(host, "2025-06-18");
    int id = 1;
    for (const json& data : {json(5), json("text"), json(true), json::array({1, "a", json::object()}),
                             json{{"a", {{"b", json::array()}}}}}) {
        host.failure = neomcp::ApplicationFailure{-32050, "m", data};
        const json answer = rig.answer(++id);
        EXPECT_EQ(answer["error"]["data"], data) << data.dump();
    }
}

TEST(ApplicationErrorsHost, AProtocolCodeAndAnyCodeOfTheReservedRangeNotLeftToImplementationsIsRefused) {
    const std::vector<int> codes = {neomcp::error_code::parse_error,    neomcp::error_code::invalid_request,
                                    neomcp::error_code::method_not_found, neomcp::error_code::invalid_params,
                                    neomcp::error_code::internal_error, -32768,
                                    -32767,                             -32500,
                                    -32100};
    for (const std::string& revision : revisions()) {
        FailingHost host;
        Rig rig(host, revision);
        int id = 1;
        for (const int code : codes) {
            host.failure = failure_of(code);
            expect_internal_error(rig.answer(++id), revision + " code " + std::to_string(code));
        }
        EXPECT_TRUE(rig.answer(900, "fine").contains("result")) << revision;
    }
}

TEST(ApplicationErrorsHost, TheEdgesOfTheRangeLeftToImplementationsAreExact) {
    for (const std::string& revision : revisions()) {
        FailingHost host;
        Rig rig(host, revision);
        host.failure = failure_of(neomcp::error_code::implementation_range_first - 1);
        expect_internal_error(rig.answer(2), revision + " -32100");
        host.failure = failure_of(neomcp::error_code::implementation_range_first);
        EXPECT_EQ(rig.answer(3)["error"]["code"], -32099) << revision;
        host.failure = failure_of(neomcp::error_code::reserved_range_first - 1);
        EXPECT_EQ(rig.answer(4)["error"]["code"], -32769) << revision;
        host.failure = failure_of(neomcp::error_code::reserved_range_first);
        expect_internal_error(rig.answer(5), revision + " -32768");
        host.failure = failure_of(neomcp::error_code::reserved_range_last + 1);
        EXPECT_EQ(rig.answer(6)["error"]["code"], -31999) << revision;
    }
}

TEST(ApplicationErrorsHost, ACodeTheRevisionNamesIsRefusedWhateverTheApplicationSays) {
    std::size_t refused = 0;
    for (const std::string& revision : revisions()) {
        FailingHost host;
        Rig rig(host, revision);
        int id = 1;
        for (const int code : named_codes(*rig.profile)) {
            host.failure = failure_of(code);
            expect_internal_error(rig.answer(++id), revision + " code " + std::to_string(code));
            ++refused;
        }
    }
    EXPECT_GT(refused, 0u) << "the profiles name no code";
}

TEST(ApplicationErrorsHost, ACodeOnlyAnotherRevisionNamesIsAllowedWhereThisOneDoesNotName) {
    // Every code any revision names is refused under that revision and sent under one that does not name it.
    std::set<int> everything;
    for (const std::string& revision : revisions()) {
        const auto named = named_codes(*neomcp::profile::builtin(revision));
        everything.insert(named.begin(), named.end());
    }
    std::size_t sent = 0;
    for (const std::string& revision : revisions()) {
        FailingHost host;
        Rig rig(host, revision);
        const auto named = named_codes(*rig.profile);
        int id = 1;
        for (const int code : everything) {
            if (named.contains(code)) continue;
            host.failure = failure_of(code);
            const json answer = rig.answer(++id);
            EXPECT_EQ(answer["error"]["code"], code) << revision << " code " << code;
            ++sent;
        }
    }
    EXPECT_GT(sent, 0u) << "every revision names the same codes";
}

TEST(ApplicationErrorsHost, TheCodesOfAnExtensionInForceAreNamedToo) {
    // The tasks extension of 2026-07-28 names a code; a profile composed with it refuses that code, whichever way the
    // core names it.
    const std::array<std::string_view, 1> extensions = {"io.modelcontextprotocol/tasks"};
    const auto composed = neomcp::profile::builtin("2026-07-28", extensions);
    ASSERT_NE(composed, nullptr);
    ASSERT_FALSE(composed->extensions().empty());
    FailingHost host;
    neomcp::SessionOptions options;
    options.profiles.push_back(composed);
    Peer peer(host, std::move(options));
    int id = 1;
    for (const int code : named_codes(*composed)) {
        host.failure = failure_of(code);
        auto answer = peer.request(Peer::make_request(*composed, ++id, "tools/call", json{{"name", "fail"}}), patience);
        ASSERT_TRUE(answer.has_value());
        expect_internal_error(*answer, "composed code " + std::to_string(code));
    }
}

TEST(ApplicationErrorsHost, TheReasonOfARefusalIsNotedOnlyWhenDetailsAreExposed) {
    FailingHost host;
    Rig plain(host, "2026-07-28");
    host.failure = failure_of(neomcp::error_code::method_not_found);
    expect_internal_error(plain.answer(2), "plain");

    Rig exposing(host, "2026-07-28", true);
    const json answer = exposing.answer(2);
    ASSERT_TRUE(answer.contains("error")) << answer.dump();
    EXPECT_EQ(answer["error"]["code"], neomcp::error_code::internal_error);
    const std::string message = answer["error"]["message"];
    EXPECT_NE(message.find("-32601"), std::string::npos) << message;
    EXPECT_EQ(message.find(secret_message), std::string::npos) << message;
    EXPECT_FALSE(answer["error"].contains("data"));
    EXPECT_EQ(answer.dump().find(secret_data), std::string::npos);
}

TEST(ApplicationErrorsHost, TheAnswerIsBoundedByTheLongestMessageAtItsEdgeAndOnePast) {
    constexpr std::size_t bound = 4096;
    for (const std::string& revision : revisions()) {
        FailingHost host;
        Rig rig(host, revision, false, bound);
        const int id = 7;
        // The length of the whole answer, as the session writes it, for a message of `size` bytes.
        const auto length_with = [&](std::size_t size) {
            return neomcp::serialize(neomcp::make_error(neomcp::RequestId(id), -32050, std::string(size, 'x'),
                                                        json{{"k", "v"}}))
                .size();
        };
        // Characters of the message that make the answer exactly `bound` bytes long.
        const std::size_t at_edge = bound - length_with(0);
        ASSERT_EQ(length_with(at_edge), bound) << revision;
        host.failure = neomcp::ApplicationFailure{-32050, std::string(at_edge, 'x'), json{{"k", "v"}}};
        const json fits = rig.answer(id);
        ASSERT_TRUE(fits.contains("error")) << revision;
        EXPECT_EQ(fits["error"]["code"], -32050) << revision << ": " << fits.dump().substr(0, 200);
        EXPECT_EQ(fits["error"]["message"].get<std::string>().size(), at_edge) << revision;
        // One byte more.
        host.failure.message.push_back('x');
        const json past = rig.answer(id + 1);
        ASSERT_TRUE(past.contains("error")) << revision;
        EXPECT_EQ(past["error"]["code"], neomcp::error_code::internal_error) << revision;
        EXPECT_EQ(past["error"]["message"], std::string(fixed_message)) << revision;
        EXPECT_LT(past.dump().size(), bound) << revision;
    }
}

TEST(ApplicationErrorsHost, AnOversizedAnswerIsNotedWhenDetailsAreExposed) {
    FailingHost host;
    Rig rig(host, "2025-06-18", true, 1024);
    host.failure = neomcp::ApplicationFailure{-32050, std::string(2000, 'y'), nullptr};
    const json answer = rig.answer(2);
    EXPECT_EQ(answer["error"]["code"], neomcp::error_code::internal_error) << answer.dump();
    const std::string message = answer["error"]["message"];
    EXPECT_NE(message.find("1024"), std::string::npos) << message;
    EXPECT_EQ(message.find("yyyy"), std::string::npos) << message;
}

TEST(ApplicationErrorsHost, ABatchMemberIsAnsweredWithItsOwnErrorAndTheOthersAreNot) {
    // Batches are read by the revision that has them.
    FailingHost host;
    const auto profile = neomcp::profile::builtin("2025-03-26");
    ASSERT_NE(profile, nullptr);
    neomcp::SessionOptions options;
    options.profiles.push_back(profile);
    Peer peer(host, std::move(options));
    ASSERT_TRUE(peer.initialize(*profile, patience).has_value());
    host.failure = failure_of(-32050);
    json batch = json::array();
    batch.push_back(Peer::make_request(*profile, 11, "tools/call", json{{"name", "fine"}}));
    batch.push_back(Peer::make_request(*profile, 12, "tools/call", json{{"name", "fail"}}));
    batch.push_back(Peer::make_request(*profile, 13, "tools/call", json{{"name", "fine"}}));
    peer.send(batch.dump());
    const auto answers = peer.next_other(patience);
    ASSERT_TRUE(answers.has_value());
    ASSERT_TRUE(answers->is_array()) << answers->dump();
    ASSERT_EQ(answers->size(), 3u) << answers->dump();
    for (const json& member : *answers) {
        if (member["id"] == 12) {
            EXPECT_EQ(member["error"]["code"], -32050);
            EXPECT_EQ(member["error"]["message"], std::string(secret_message));
            EXPECT_EQ(member["error"]["data"], json({{"note", std::string(secret_data)}}));
        } else {
            EXPECT_TRUE(member.contains("result")) << member.dump();
        }
    }
    // A refused member of a batch is an internal error of its own, too.
    host.failure = failure_of(neomcp::error_code::invalid_params);
    batch[1]["id"] = 22;
    batch[0]["id"] = 21;
    batch[2]["id"] = 23;
    peer.send(batch.dump());
    const auto second = peer.next_other(patience);
    ASSERT_TRUE(second.has_value());
    ASSERT_EQ(second->size(), 3u);
    for (const json& member : *second) {
        if (member["id"] == 22) {
            expect_internal_error(member, "batch member");
        } else {
            EXPECT_TRUE(member.contains("result")) << member.dump();
        }
    }
}

}  // namespace
