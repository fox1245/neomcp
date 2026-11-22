// Tool use and tool result blocks in sampling: their writers and readers, the revisions that carry them, the rules the
// pages give the messages of a conversation with tools, and the capability such a request needs.
//
// Which revisions carry the blocks is read from the profiles (`profile.type(...)`, the union of sampling blocks),
// never from a list of revisions.
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "core/input.h"
#include "neomcp/mechanism.h"
#include "neomcp/wire.h"
#include "neomcp/wire_read.h"
#include "support.h"

namespace {

using namespace neomcp;
using client_core_test::Fill;
using client_core_test::Generator;
using client_core_test::same;

constexpr std::string_view sampling = "sampling/createMessage";

std::vector<profile::ProfilePtr> all_profiles() {
    std::vector<profile::ProfilePtr> out;
    for (const std::string_view revision : profile::builtin_revisions()) out.push_back(profile::builtin(revision));
    return out;
}

bool sampling_takes(const profile::Profile& profile, std::string_view type) {
    const profile::Union* blocks = profile.union_type("SamplingMessageContentBlock");
    if (blocks == nullptr || profile.type(type) == nullptr) return false;
    return std::find(blocks->alternatives.begin(), blocks->alternatives.end(), type) != blocks->alternatives.end();
}

bool takes_tools(const profile::Profile& profile) {
    return sampling_takes(profile, "ToolUseContent") && sampling_takes(profile, "ToolResultContent");
}

std::unique_ptr<InputMechanism> mechanism_of(const profile::Profile& profile) {
    return builtin_mechanisms().make_input(profile.mechanism(profile::Slot::server_input));
}

// The profiles whose sampling a server can ask for, split by whether it carries tool blocks.
std::vector<profile::ProfilePtr> asking_profiles(bool with_tools) {
    std::vector<profile::ProfilePtr> out;
    for (const auto& profile : all_profiles()) {
        const auto mechanism = mechanism_of(*profile);
        if (mechanism && mechanism->carries(*profile, sampling) && takes_tools(*profile) == with_tools) {
            out.push_back(profile);
        }
    }
    return out;
}

ToolUseContent use_of(std::string id) {
    ToolUseContent use;
    use.id = std::move(id);
    use.name = "add";
    use.input = json{{"a", 2}, {"b", 3}};
    return use;
}

ToolResultContent result_of(std::string id, std::string text = "5") {
    ToolResultContent result;
    result.tool_use_id = std::move(id);
    TextContent block;
    block.text = std::move(text);
    result.content.emplace_back(std::move(block));
    return result;
}

SamplingMessage message_of(Role role, std::vector<SamplingContent> content) {
    SamplingMessage message;
    message.role = role;
    message.content = std::move(content);
    return message;
}

TextContent text_of(std::string text) {
    TextContent block;
    block.text = std::move(text);
    return block;
}

// A request of two rounds' history: the user asks, the model uses a tool, the user gives the result.
SamplingRequest tool_round() {
    SamplingRequest request;
    request.max_tokens = 50;
    request.messages.push_back(message_of(Role::user, {text_of("What is 2 + 3?")}));
    request.messages.push_back(message_of(Role::assistant, {use_of("call-1")}));
    request.messages.push_back(message_of(Role::user, {result_of("call-1")}));
    ToolDefinition tool;
    tool.name = "add";
    request.tools.push_back(std::move(tool));
    return request;
}

// ---- The blocks through their writer and reader

template <typename T>
void round_trips(const profile::Profile& profile, const T& original) {
    const json written = to_wire(original, profile);
    for (const json& sent : {written, json::parse(written.dump())}) {
        auto read = from_wire<T>(sent, profile);
        const T* value = std::get_if<T>(&read);
        ASSERT_NE(value, nullptr) << std::get<std::string>(read) << "\nwire: " << written.dump();
        EXPECT_TRUE(same(*value, original)) << "wire: " << written.dump();
        EXPECT_EQ(to_wire(*value, profile), written);
    }
}

TEST(SamplingTools, EachBlockRoundTripsWhereTheRevisionDefinesIt) {
    std::size_t checked = 0;
    for (const auto& profile : all_profiles()) {
        if (profile->type("ToolUseContent") == nullptr) continue;
        SCOPED_TRACE(profile->revision());
        std::vector<std::pair<Fill, std::uint64_t>> sources{{Fill::none, 1}, {Fill::all, 2}};
        for (std::uint64_t i = 0; i < 60; ++i) sources.emplace_back(Fill::any, 300 + i);
        for (const auto& [fill, seed] : sources) {
            Generator g(*profile, seed, fill, true);
            ToolUseContent use = g.tool_use();
            use.id = g.text() + "-id";
            round_trips(*profile, use);
            ToolResultContent result = g.tool_result_block();
            result.tool_use_id = g.text() + "-id";
            round_trips(*profile, result);
            ++checked;
        }
    }
    EXPECT_GT(checked, 0U);
}

TEST(SamplingTools, TheWireFormHasEveryMemberOfTheSchemasTypes) {
    for (const auto& profile : all_profiles()) {
        if (profile->type("ToolResultContent") == nullptr) continue;
        SCOPED_TRACE(profile->revision());
        ToolUseContent use = use_of("call-7");
        use.meta = json{{"cache", "k"}};
        const json used = to_wire(use, *profile);
        EXPECT_EQ(used, (json{{"type", "tool_use"}, {"id", "call-7"}, {"name", "add"}, {"input", {{"a", 2}, {"b", 3}}},
                              {"_meta", {{"cache", "k"}}}}));
        ToolResultContent result = result_of("call-7");
        result.structured_content = json{{"sum", 5}};
        result.is_error = true;
        const json answered = to_wire(result, *profile);
        EXPECT_EQ(answered, (json{{"type", "tool_result"},
                                  {"toolUseId", "call-7"},
                                  {"content", json::array({json{{"type", "text"}, {"text", "5"}}})},
                                  {"structuredContent", {{"sum", 5}}},
                                  {"isError", true}}));
        // The nested blocks are read, not skipped.
        auto read = from_wire<ToolResultContent>(answered, *profile);
        ASSERT_TRUE(std::holds_alternative<ToolResultContent>(read)) << std::get<std::string>(read);
        const ToolResultContent& back = std::get<ToolResultContent>(read);
        ASSERT_EQ(back.content.size(), 1U);
        EXPECT_EQ(std::get<TextContent>(back.content.front()).text, "5");
        EXPECT_EQ(back.tool_use_id, "call-7");
        EXPECT_TRUE(back.is_error);
    }
}

TEST(SamplingTools, TheReadersRefuseWhatTheTypesDoNotAllow) {
    for (const auto& profile : all_profiles()) {
        SCOPED_TRACE(profile->revision());
        const json use{{"type", "tool_use"}, {"id", "x"}, {"name", "n"}, {"input", json::object()}};
        const json result{{"type", "tool_result"}, {"toolUseId", "x"}, {"content", json::array()}};
        if (profile->type("ToolUseContent") == nullptr) {
            EXPECT_TRUE(std::holds_alternative<std::string>(from_wire<ToolUseContent>(use, *profile)));
            EXPECT_TRUE(std::holds_alternative<std::string>(from_wire<ToolResultContent>(result, *profile)));
            continue;
        }
        EXPECT_TRUE(std::holds_alternative<ToolUseContent>(from_wire<ToolUseContent>(use, *profile)));
        EXPECT_TRUE(std::holds_alternative<ToolResultContent>(from_wire<ToolResultContent>(result, *profile)));
        for (const char* required : {"id", "name", "input"}) {
            json lacking = use;
            lacking.erase(required);
            const auto read = from_wire<ToolUseContent>(lacking, *profile);
            ASSERT_TRUE(std::holds_alternative<std::string>(read)) << required;
            EXPECT_EQ(std::get<std::string>(read), std::string("$.") + required + ": is required");
        }
        for (const char* required : {"toolUseId", "content"}) {
            json lacking = result;
            lacking.erase(required);
            EXPECT_TRUE(std::holds_alternative<std::string>(from_wire<ToolResultContent>(lacking, *profile))) << required;
        }
        json not_object = use;
        not_object["input"] = json::array();
        EXPECT_TRUE(std::holds_alternative<std::string>(from_wire<ToolUseContent>(not_object, *profile)));
        // A block's type is its own.
        EXPECT_TRUE(std::holds_alternative<std::string>(from_wire<ToolUseContent>(result, *profile)));
        EXPECT_TRUE(std::holds_alternative<std::string>(from_wire<ToolResultContent>(use, *profile)));
        // A tool result holds the blocks a tool's result holds: never a tool use.
        json nested = result;
        nested["content"] = json::array({use});
        EXPECT_TRUE(std::holds_alternative<std::string>(from_wire<ToolResultContent>(nested, *profile)));
        // Neither is a block of a tool's result or a prompt.
        EXPECT_TRUE(std::holds_alternative<std::string>(from_wire<ContentBlock>(use, *profile)));
        EXPECT_TRUE(std::holds_alternative<std::string>(from_wire<ContentBlock>(result, *profile)));
    }
}

// ---- Requests and answers under the revisions that carry the blocks, and under the ones that do not

TEST(SamplingTools, ARequestWithToolBlocksTravelsWhereTheRevisionCarriesThem) {
    const auto profiles = asking_profiles(true);
    ASSERT_FALSE(profiles.empty());
    for (const auto& profile : profiles) {
        SCOPED_TRACE(profile->revision());
        const auto mechanism = mechanism_of(*profile);
        SamplingRequest original = tool_round();
        original.tool_choice = json{{"mode", "auto"}};
        const auto written = detail::write_params(original, *profile, *mechanism);
        ASSERT_TRUE(std::holds_alternative<json>(written)) << std::get<std::string>(written);
        const json& params = std::get<json>(written);
        EXPECT_EQ(params.at("messages").at(1).at("content").at("id"), "call-1");
        EXPECT_EQ(params.at("messages").at(2).at("content").at("toolUseId"), "call-1");
        EXPECT_EQ(params.at("tools").at(0).at("name"), "add");
        auto read = detail::read_request(sampling, params, *profile, *mechanism);
        ASSERT_TRUE(std::holds_alternative<InputRequest>(read)) << std::get<std::string>(read);
        const auto& back = std::get<SamplingRequest>(std::get<InputRequest>(read));
        ASSERT_EQ(back.messages.size(), 3U);
        const auto* use = std::get_if<ToolUseContent>(&back.messages[1].content.at(0));
        ASSERT_NE(use, nullptr);
        EXPECT_EQ(use->id, "call-1");
        EXPECT_EQ(use->name, "add");
        EXPECT_EQ(use->input, (json{{"a", 2}, {"b", 3}}));
        const auto* result = std::get_if<ToolResultContent>(&back.messages[2].content.at(0));
        ASSERT_NE(result, nullptr);
        EXPECT_EQ(result->tool_use_id, "call-1");
        ASSERT_EQ(result->content.size(), 1U);
        EXPECT_EQ(std::get<TextContent>(result->content.front()).text, "5");
        EXPECT_EQ(back.tool_choice, original.tool_choice);
    }
}

TEST(SamplingTools, ARevisionWithoutThemNeitherWritesNorReadsThem) {
    const auto profiles = asking_profiles(false);
    ASSERT_FALSE(profiles.empty());
    const json use{{"type", "tool_use"}, {"id", "call-1"}, {"name", "add"}, {"input", json::object()}};
    const json result{{"type", "tool_result"}, {"toolUseId", "call-1"}, {"content", json::array()}};
    for (const auto& profile : profiles) {
        SCOPED_TRACE(profile->revision());
        const auto mechanism = mechanism_of(*profile);
        // The messages alone, without any tools offered: still not written, and the request is not one the client can
        // take.
        SamplingRequest request = tool_round();
        request.tools.clear();
        EXPECT_TRUE(std::holds_alternative<std::string>(detail::write_params(request, *profile, *mechanism)));
        EXPECT_FALSE(detail::input_supported(request, json{{"sampling", {{"tools", json::object()}}}}, *profile,
                                             *mechanism));
        // A request that holds them, as another server may send it, is refused.
        const json params{{"messages", json::array({json{{"role", "assistant"}, {"content", use}},
                                                    json{{"role", "user"}, {"content", result}}})},
                          {"maxTokens", 10}};
        EXPECT_TRUE(std::holds_alternative<std::string>(detail::read_request(sampling, params, *profile, *mechanism)));
        // An answer with a tool use is neither written nor read.
        SamplingAnswer answer;
        answer.model = "m";
        answer.content.emplace_back(use_of("call-1"));
        EXPECT_TRUE(std::holds_alternative<std::string>(detail::write_answer(InputResponse(answer), *profile, *mechanism)));
        const json sent{{"role", "assistant"}, {"content", use}, {"model", "m"}};
        EXPECT_TRUE(std::holds_alternative<std::string>(detail::read_answer(sampling, sent, *profile)));
    }
}

TEST(SamplingTools, AnAnswerThatUsesAToolIsWrittenAndReadBack) {
    for (const auto& profile : asking_profiles(true)) {
        SCOPED_TRACE(profile->revision());
        const auto mechanism = mechanism_of(*profile);
        SamplingAnswer answer;
        answer.model = "m";
        answer.content.emplace_back(use_of("call-1"));
        answer.content.emplace_back(use_of("call-2"));
        answer.stop_reason = std::string(stop_reasons::tool_use);
        const auto written = detail::write_answer(InputResponse(answer), *profile, *mechanism);
        ASSERT_TRUE(std::holds_alternative<json>(written)) << std::get<std::string>(written);
        const json& result = std::get<json>(written);
        EXPECT_EQ(result.at("stopReason"), "toolUse");
        ASSERT_TRUE(result.at("content").is_array());
        EXPECT_EQ(result.at("content").at(1).at("id"), "call-2");
        auto read = detail::read_answer(sampling, result, *profile);
        ASSERT_TRUE(std::holds_alternative<InputResponse>(read)) << std::get<std::string>(read);
        const auto& back = std::get<SamplingAnswer>(std::get<InputResponse>(read));
        ASSERT_EQ(back.content.size(), 2U);
        EXPECT_TRUE(same(back.content[0], answer.content[0]));
        EXPECT_TRUE(same(back.content[1], answer.content[1]));
        EXPECT_EQ(back.stop_reason, answer.stop_reason);
    }
}

// ---- The rules of a conversation with tools

TEST(SamplingTools, MessagesThatBreakTheRulesOfToolsAreNeitherSentNorAccepted) {
    struct Case {
        const char* what;
        std::vector<SamplingMessage> messages;
    };
    const SamplingMessage ask = message_of(Role::user, {text_of("go")});
    const std::vector<Case> cases{
        {"results mixed with text", {ask, message_of(Role::assistant, {use_of("a")}),
                                     message_of(Role::user, {result_of("a"), text_of("and more")})}},
        {"a use not answered", {ask, message_of(Role::assistant, {use_of("a"), use_of("b")}),
                                message_of(Role::user, {result_of("a")})}},
        {"a result that answers no use", {ask, message_of(Role::assistant, {use_of("a")}),
                                          message_of(Role::user, {result_of("a"), result_of("z")})}},
        {"a result of another id", {ask, message_of(Role::assistant, {use_of("a")}), message_of(Role::user, {result_of("b")})}},
        {"another message before the results", {ask, message_of(Role::assistant, {use_of("a")}), ask,
                                                message_of(Role::user, {result_of("a")})}},
        {"uses never answered", {ask, message_of(Role::assistant, {use_of("a")})}},
        {"results without uses", {ask, message_of(Role::user, {result_of("a")})}},
        {"results of the assistant", {ask, message_of(Role::assistant, {use_of("a")}),
                                      message_of(Role::assistant, {result_of("a")})}},
        {"uses of the user", {message_of(Role::user, {use_of("a")}), message_of(Role::user, {result_of("a")})}},
        {"two uses with one id", {ask, message_of(Role::assistant, {use_of("a"), use_of("a")}),
                                  message_of(Role::user, {result_of("a"), result_of("a")})}},
    };
    for (const auto& profile : asking_profiles(true)) {
        SCOPED_TRACE(profile->revision());
        const auto mechanism = mechanism_of(*profile);
        // The rules hold: both ways work.
        const SamplingRequest valid = tool_round();
        const auto written = detail::write_params(valid, *profile, *mechanism);
        ASSERT_TRUE(std::holds_alternative<json>(written)) << std::get<std::string>(written);
        for (const Case& item : cases) {
            SCOPED_TRACE(item.what);
            SamplingRequest request = valid;
            request.messages = item.messages;
            const auto refused = detail::write_params(request, *profile, *mechanism);
            ASSERT_TRUE(std::holds_alternative<std::string>(refused));
            EXPECT_TRUE(std::get<std::string>(refused).rfind("messages[", 0) == 0) << std::get<std::string>(refused);
            // The same messages as another server may send them.
            json params = std::get<json>(written);
            json messages = json::array();
            for (const SamplingMessage& message : item.messages) {
                json blocks = json::array();
                for (const SamplingContent& block : message.content) {
                    blocks.push_back(detail::write_sampling_block(block, *profile));
                }
                messages.push_back(json{{"role", message.role == Role::user ? "user" : "assistant"}, {"content", blocks}});
            }
            params["messages"] = std::move(messages);
            const auto read = detail::read_request(sampling, params, *profile, *mechanism);
            ASSERT_TRUE(std::holds_alternative<std::string>(read));
            // The description repeats nothing the messages hold.
            EXPECT_EQ(std::get<std::string>(read).find("and more"), std::string::npos);
        }
    }
}

// ---- The capability

TEST(SamplingTools, ARequestThatHoldsToolBlocksNeedsTheToolsCapability) {
    SamplingRequest request = tool_round();
    request.tools.clear();  // the blocks alone make it a request with tools
    EXPECT_EQ(detail::required_capabilities(request), (json{{"sampling", {{"tools", json::object()}}}}));
    EXPECT_FALSE(detail::capabilities_declare(request, json{{"sampling", json::object()}}));
    EXPECT_TRUE(detail::capabilities_declare(request, json{{"sampling", {{"tools", json::object()}}}}));
    SamplingRequest plain;
    plain.messages.push_back(message_of(Role::user, {text_of("hi")}));
    EXPECT_TRUE(detail::capabilities_declare(plain, json{{"sampling", json::object()}}));
    for (const auto& profile : asking_profiles(true)) {
        SCOPED_TRACE(profile->revision());
        const auto mechanism = mechanism_of(*profile);
        EXPECT_FALSE(detail::input_supported(request, json{{"sampling", json::object()}}, *profile, *mechanism));
        EXPECT_TRUE(detail::input_supported(request, json{{"sampling", {{"tools", json::object()}}}}, *profile, *mechanism));
    }
    // A client declares tools only where the revision's sampling has them.
    const std::vector<InputRequest> kinds{InputRequest(tool_round())};
    for (const auto& profile : all_profiles()) {
        SCOPED_TRACE(profile->revision());
        const json declared = detail::declared_capabilities(kinds, false, *profile);
        const profile::Type* params = profile->type("CreateMessageRequestParams");
        const bool tools = params != nullptr && params->has_property("tools");
        EXPECT_EQ(declared.contains("sampling") && declared.at("sampling").contains("tools"), tools) << declared.dump();
    }
}

}  // namespace
