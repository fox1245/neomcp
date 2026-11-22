// Sampling with tools in two rounds, for the tests that run it end to end: a server's tool asks the client's model with
// a tool to use; the model (a stand-in on the client) asks to use it; the server runs the tool and asks again with the
// history and the tool's result; the model answers with text, and the tool returns that text.
//
// The text the tool returns names the id of the tool use as the second round gave it back and the content of the tool
// result as the client read it, so that a run that loses either does not return tool_loop_expected.
#pragma once

#include <string>
#include <string_view>
#include <variant>

#include "neomcp/input.h"
#include "neomcp/server.h"

namespace sampling_tool_loop {

inline constexpr std::string_view tool_name = "sampling_tool_loop";
inline constexpr std::string_view question = "What is 2 + 3?";
inline constexpr std::string_view tool_offered = "add";
inline constexpr std::string_view use_id = "call-add-1";
inline constexpr std::string_view model_name = "tool-loop-model";
// What the tool returns when both rounds carried everything.
inline constexpr std::string_view expected = "the tool call-add-1 gave 5";

inline neomcp::TextContent text(std::string value) {
    neomcp::TextContent block;
    block.text = std::move(value);
    return block;
}

// The first round: the question, and the tool the model may use.
inline neomcp::SamplingRequest first_round() {
    neomcp::SamplingRequest request;
    neomcp::SamplingMessage asked;
    asked.role = neomcp::Role::user;
    asked.content.emplace_back(text(std::string(question)));
    request.messages.push_back(std::move(asked));
    request.max_tokens = 100;
    neomcp::ToolDefinition tool;
    tool.name = std::string(tool_offered);
    tool.description = "Add two integers";
    tool.input_schema = neomcp::json::parse(
        R"({"type":"object","properties":{"a":{"type":"integer"},"b":{"type":"integer"}},"required":["a","b"]})");
    request.tools.push_back(std::move(tool));
    return request;
}

// The tool the server runs for a tool use: the sum of `a` and `b`.
inline neomcp::ToolResultContent run_tool(const neomcp::ToolUseContent& use) {
    neomcp::ToolResultContent result;
    result.tool_use_id = use.id;
    const neomcp::json& input = use.input;
    const bool numbers = input.contains("a") && input.contains("b") && input.at("a").is_number_integer() &&
                         input.at("b").is_number_integer();
    if (use.name != tool_offered || !numbers) {
        result.is_error = true;
        result.content.emplace_back(text("no such tool or arguments"));
        return result;
    }
    result.content.emplace_back(text(std::to_string(input.at("a").get<int>() + input.at("b").get<int>())));
    return result;
}

// The second round: the history with the model's tool uses and the result of each, and no more tools to use.
inline neomcp::Question<neomcp::SamplingAnswer> second_round(const neomcp::Answer<neomcp::SamplingAnswer>& used) {
    if (!used.accepted()) return neomcp::no_question<neomcp::SamplingAnswer>();
    neomcp::SamplingRequest request = first_round();
    neomcp::SamplingMessage uses;
    uses.role = neomcp::Role::assistant;
    neomcp::SamplingMessage results;
    results.role = neomcp::Role::user;
    for (const neomcp::SamplingContent& block : used->content) {
        if (const auto* use = std::get_if<neomcp::ToolUseContent>(&block)) {
            uses.content.push_back(*use);
            results.content.emplace_back(run_tool(*use));
        }
    }
    // A model that used no tool answered already.
    if (results.content.empty()) return neomcp::no_question(*used);
    request.messages.push_back(std::move(uses));
    request.messages.push_back(std::move(results));
    request.tool_choice = neomcp::json{{"mode", "none"}};
    return request;
}

inline std::string text_of(const neomcp::SamplingAnswer& answer) {
    for (const neomcp::SamplingContent& block : answer.content) {
        if (const auto* found = std::get_if<neomcp::TextContent>(&block)) return found->text;
    }
    return "(no text)";
}

inline void register_tool(neomcp::Server& server) {
    server.tool(
        std::string(tool_name), "Ask the client's model with a tool to use, then again with the tool's result",
        [](const neomcp::Answer<neomcp::SamplingAnswer>&, const neomcp::Answer<neomcp::SamplingAnswer>& final_answer) {
            if (!final_answer.accepted()) return std::string("(not answered)");
            return text_of(*final_answer);
        },
        neomcp::sample("use", [] { return first_round(); }),
        neomcp::sample("final", [](const neomcp::Answer<neomcp::SamplingAnswer>& used) { return second_round(used); }));
}

// The client's model: with tools to use and no result yet, it asks to use the first tool; given tool results, it
// answers with text that names the result it was given. Any other request is answered with `plain`.
inline neomcp::SamplingAnswer model(const neomcp::SamplingRequest& request, std::string_view plain = "plain") {
    neomcp::SamplingAnswer answer;
    answer.model = std::string(model_name);
    if (!request.messages.empty()) {
        for (const neomcp::SamplingContent& block : request.messages.back().content) {
            const auto* result = std::get_if<neomcp::ToolResultContent>(&block);
            if (result == nullptr) continue;
            std::string given = "(empty)";
            if (!result->content.empty()) {
                if (const auto* said = std::get_if<neomcp::TextContent>(&result->content.front())) given = said->text;
            }
            answer.content.emplace_back(text("the tool " + result->tool_use_id + " gave " + given));
            answer.stop_reason = std::string(neomcp::stop_reasons::end_turn);
            return answer;
        }
    }
    if (!request.tools.empty()) {
        neomcp::ToolUseContent use;
        use.id = std::string(use_id);
        use.name = request.tools.front().name;
        use.input = neomcp::json{{"a", 2}, {"b", 3}};
        answer.content.emplace_back(std::move(use));
        answer.stop_reason = std::string(neomcp::stop_reasons::tool_use);
        return answer;
    }
    answer.content.emplace_back(text(std::string(plain)));
    return answer;
}

}  // namespace sampling_tool_loop
