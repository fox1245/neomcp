// The rounds of a call with declared inputs: which answers of the last round count, and what a round asks.
//
// The state of such a call is the handler's own state (InputRequired::state): a JSON object with the format's version
// and the questions asked in the round that issued it (key -> digest). A digest is the SHA-256 of the canonical JSON
// of everything that makes the question the one it is, so that processes that share a key for sealed state compute
// the same one. The state comes back unchanged or not at all: where it travels it is sealed, and a state that was
// changed is refused before the handler is entered.
#include "neomcp/server/inputs.h"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string_view>
#include <type_traits>
#include <utility>

#include "core/input.h"
#include "core/request_state.h"
#include "neomcp/wire.h"

namespace neomcp::detail {
namespace {

constexpr std::int64_t state_version = 1;
constexpr std::string_view version_member = "v";
constexpr std::string_view asked_member = "q";

const json* member(const json& object, std::string_view member_name) {
    const auto* members = object.get_ptr<const json::object_t*>();
    if (members == nullptr) return nullptr;
    const auto found = members->find(std::string(member_name));
    return found == members->end() ? nullptr : &found->second;
}

std::string_view role_text(Role role) noexcept { return role == Role::user ? "user" : "assistant"; }

// Everything that makes a question the one it is, for its digest. Content blocks and tools are written as the call's
// revision writes them.
json question_json(const InputRequest& request, const profile::Profile& profile) {
    json rendered = json::object();
    rendered["method"] = std::string(method_of(request));
    std::visit(
        [&](const auto& asked) {
            using T = std::decay_t<decltype(asked)>;
            if constexpr (std::is_same_v<T, ElicitationForm>) {
                rendered["message"] = asked.message;
                rendered["requestedSchema"] = asked.requested_schema;
            } else if constexpr (std::is_same_v<T, ElicitationUrl>) {
                rendered["message"] = asked.message;
                rendered["url"] = asked.url;
                if (asked.elicitation_id) rendered["elicitationId"] = *asked.elicitation_id;
            } else if constexpr (std::is_same_v<T, SamplingRequest>) {
                json messages = json::array();
                for (const SamplingMessage& message : asked.messages) {
                    json blocks = json::array();
                    for (const ContentBlock& block : message.content) blocks.push_back(to_wire(block, profile));
                    json entry = json::object();
                    entry["role"] = std::string(role_text(message.role));
                    entry["content"] = std::move(blocks);
                    messages.push_back(std::move(entry));
                }
                rendered["messages"] = std::move(messages);
                rendered["maxTokens"] = asked.max_tokens;
                if (asked.system_prompt) rendered["systemPrompt"] = *asked.system_prompt;
                if (asked.model_preferences) {
                    const ModelPreferences& preferences = *asked.model_preferences;
                    json written = json::object();
                    json hints = json::array();
                    for (const std::string& hint : preferences.hints) hints.push_back(hint);
                    written["hints"] = std::move(hints);
                    if (preferences.cost_priority) written["costPriority"] = *preferences.cost_priority;
                    if (preferences.speed_priority) written["speedPriority"] = *preferences.speed_priority;
                    if (preferences.intelligence_priority) {
                        written["intelligencePriority"] = *preferences.intelligence_priority;
                    }
                    rendered["modelPreferences"] = std::move(written);
                }
                if (asked.temperature) rendered["temperature"] = *asked.temperature;
                json sequences = json::array();
                for (const std::string& sequence : asked.stop_sequences) sequences.push_back(sequence);
                rendered["stopSequences"] = std::move(sequences);
                if (asked.include_context) rendered["includeContext"] = static_cast<std::int64_t>(*asked.include_context);
                if (asked.metadata) rendered["metadata"] = *asked.metadata;
                json tools = json::array();
                for (const ToolDefinition& tool : asked.tools) tools.push_back(to_wire(tool, profile));
                rendered["tools"] = std::move(tools);
                if (asked.tool_choice) rendered["toolChoice"] = *asked.tool_choice;
            }
        },
        request);
    return rendered;
}

const char* kind_name(InputKind kind) noexcept {
    switch (kind) {
        case InputKind::form: return "a form";
        case InputKind::sampling: return "a sampling request";
        case InputKind::roots: return "a roots request";
    }
    return "its request";
}

bool same_kind(InputKind kind, const InputResponse& response) noexcept {
    switch (kind) {
        case InputKind::form: return std::holds_alternative<ElicitationAnswer>(response);
        case InputKind::sampling: return std::holds_alternative<SamplingAnswer>(response);
        case InputKind::roots: return std::holds_alternative<RootsAnswer>(response);
    }
    return false;
}

}  // namespace

void prepare_inputs(std::span<DeclaredInput> inputs, const std::string& label, const json_schema::Limits& limits) {
    std::set<std::string, std::less<>> keys;
    for (DeclaredInput& declared : inputs) {
        if (declared.key.empty()) throw std::invalid_argument("neomcp: " + label + ": a declared input needs a key");
        if (!keys.insert(declared.key).second) {
            throw std::invalid_argument("neomcp: " + label + ": two declared inputs have the key '" + declared.key + "'");
        }
        if (declared.kind != InputKind::form) continue;
        if (const auto problem = form_schema_problem(declared.form_schema)) {
            throw std::invalid_argument("neomcp: " + label + ": the form of the input '" + declared.key +
                                        "' cannot be built from its struct: " + *problem);
        }
        auto compiled = json_schema::Validator::compile(declared.form_schema, limits);
        if (const auto* issue = std::get_if<json_schema::Issue>(&compiled)) {
            throw std::invalid_argument("neomcp: " + label + ": the validator refuses the form of the input '" +
                                        declared.key + "' at \"" + issue->schema_pointer + "\": " + issue->message);
        }
        declared.form_check = std::get<json_schema::Validator>(std::move(compiled));
    }
}

InputRound::InputRound(const Context& context) : context_(context) {
    const std::optional<std::string>& state = context.request_state();
    if (!state) return;  // no round before
    // Only this layer writes the state of a call with declared inputs, and it comes back unchanged; one that cannot be
    // read (another format, say) is no round before, and the answers that came with it answer nothing.
    const json parsed = json::parse(*state, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) return;
    const json* version = member(parsed, version_member);
    if (version == nullptr || !version->is_number_integer() || version->get<std::int64_t>() != state_version) return;
    const json* asked = member(parsed, asked_member);
    const auto* asked_entries = asked == nullptr ? nullptr : asked->get_ptr<const json::object_t*>();
    if (asked_entries == nullptr) return;
    std::map<std::string, std::string> asked_before;
    for (const auto& [asked_key, digest] : *asked_entries) {
        const auto* text = digest.get_ptr<const std::string*>();
        if (text == nullptr) return;
        asked_before.emplace(asked_key, *text);
    }
    asked_before_ = std::move(asked_before);
}

Settlement InputRound::settle(const DeclaredInput& input, InputRequest question) {
    const std::string digest = arguments_digest(question_json(question, context_.control_.profile()));
    Settlement found;
    // The answer of the last round, when it answers exactly this question.
    const InputResponses& responses = context_.input_responses();
    const auto given = responses.find(input.key);
    const auto before = asked_before_.find(input.key);
    const bool this_question = before != asked_before_.end() && before->second == digest;
    if (given != responses.end() && this_question) {
        InputResponse answer = given->second;
        if (!same_kind(input.kind, answer)) {
            throw AnswerRefused("the answer " + input.key + " is not an answer to " + kind_name(input.kind));
        }
        if (auto* elicited = std::get_if<ElicitationAnswer>(&answer)) {
            if (elicited->action != ElicitationAction::accept) {
                elicited->content = json::object();  // only an accepted form carries content
            } else if (const auto issue = input.form_check->validate(elicited->content)) {
                throw AnswerRefused("the answer " + input.key + " does not fit its form: at \"" + issue->instance_pointer +
                                    "\": " + issue->message);
            }
        }
        found.answer = std::move(answer);
        return found;
    }
    if (input.is_optional && !context_.client_supports(question)) {
        found.not_asked = true;
        return found;
    }
    asking_.insert_or_assign(input.key, digest);
    requests_.insert_or_assign(input.key, std::move(question));
    return found;
}

InputRequired InputRound::ask() const {
    json asked = json::object();
    for (const auto& [asked_key, digest] : asking_) asked[asked_key] = digest;
    json state = json::object();
    state[std::string(version_member)] = state_version;
    state[std::string(asked_member)] = std::move(asked);
    InputRequired required;
    required.requests = requests_;
    required.state = serialize(state);
    return required;
}

}  // namespace neomcp::detail
