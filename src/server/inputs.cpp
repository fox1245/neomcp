// The rounds of a call with declared inputs: which outcomes the earlier rounds recorded, which answers of the last
// round count, and what a round asks.
//
// The state of such a call is the handler's own state (InputRequired::state): a JSON object with the format's version,
// the questions asked in the round that issued it (key -> digest) and the outcomes recorded so far (key -> the digest
// of the question answered, the method of the request and the answer as the client wrote it). A digest is the SHA-256
// of the canonical JSON of everything that makes the question the one it is, so that processes that share a key for
// sealed state compute the same one. The state comes back unchanged or not at all: where it travels it is sealed, and
// a state that was changed is refused before the handler is entered.
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
constexpr std::string_view outcomes_member = "o";
constexpr std::string_view digest_member = "d";
constexpr std::string_view method_member = "m";
constexpr std::string_view answer_member = "a";

const json* member(const json& object, std::string_view member_name) {
    const auto* members = object.get_ptr<const json::object_t*>();
    if (members == nullptr) return nullptr;
    const auto found = members->find(std::string(member_name));
    return found == members->end() ? nullptr : &found->second;
}

const std::string* text_member(const json& object, std::string_view member_name) {
    const json* found = member(object, member_name);
    return found == nullptr ? nullptr : found->get_ptr<const std::string*>();
}

std::string_view role_text(Role role) noexcept { return role == Role::user ? "user" : "assistant"; }

std::string_view action_text(ElicitationAction action) noexcept {
    switch (action) {
        case ElicitationAction::accept: return "accept";
        case ElicitationAction::decline: return "decline";
        case ElicitationAction::cancel: return "cancel";
    }
    return "cancel";
}

// The answer as a client writes it, which read_answer reads back.
json answer_json(const InputResponse& response) {
    return std::visit(
        [](const auto& answer) -> json {
            using T = std::decay_t<decltype(answer)>;
            json written = json::object();
            if constexpr (std::is_same_v<T, ElicitationAnswer>) {
                written["action"] = std::string(action_text(answer.action));
                if (answer.action == ElicitationAction::accept) written["content"] = answer.content;
            } else if constexpr (std::is_same_v<T, SamplingAnswer>) {
                written["role"] = std::string(role_text(answer.role));
                written["content"] = answer.content;
                written["model"] = answer.model;
                if (answer.stop_reason) written["stopReason"] = *answer.stop_reason;
            } else {
                json listed = json::array();
                for (const Root& root : answer.roots) {
                    json entry = json::object();
                    entry["uri"] = root.uri;
                    if (root.name) entry["name"] = *root.name;
                    if (root.meta) entry["_meta"] = *root.meta;
                    listed.push_back(std::move(entry));
                }
                written["roots"] = std::move(listed);
            }
            return written;
        },
        response);
}

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

InputRound::InputRound(const Context& context, std::size_t max_state_bytes)
    : context_(context), max_state_bytes_(max_state_bytes) {
    const std::optional<std::string>& state = context.request_state();
    if (!state) return;  // no earlier rounds
    // Only this layer writes the state of a call with declared inputs, and it comes back unchanged; one that cannot be
    // read (another format, say) is no earlier rounds, and the answers that came with it answer nothing.
    const json parsed = json::parse(*state, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) return;
    const json* version = member(parsed, version_member);
    if (version == nullptr || !version->is_number_integer() || version->get<std::int64_t>() != state_version) return;
    const json* asked = member(parsed, asked_member);
    const json* outcomes = member(parsed, outcomes_member);
    const auto* asked_entries = asked == nullptr ? nullptr : asked->get_ptr<const json::object_t*>();
    const auto* outcome_entries = outcomes == nullptr ? nullptr : outcomes->get_ptr<const json::object_t*>();
    if (asked_entries == nullptr || outcome_entries == nullptr) return;
    std::map<std::string, std::string> asked_before;
    std::map<std::string, Recorded> recorded;
    for (const auto& [asked_key, digest] : *asked_entries) {
        const auto* text = digest.get_ptr<const std::string*>();
        if (text == nullptr) return;
        asked_before.emplace(asked_key, *text);
    }
    for (const auto& [outcome_key, outcome] : *outcome_entries) {
        const std::string* digest = text_member(outcome, digest_member);
        const std::string* method = text_member(outcome, method_member);
        const json* answer = member(outcome, answer_member);
        if (digest == nullptr || method == nullptr || answer == nullptr) return;
        auto read = read_answer(*method, *answer);
        if (!std::holds_alternative<InputResponse>(read)) return;
        recorded.emplace(outcome_key, Recorded{*digest, *method, std::get<InputResponse>(std::move(read))});
    }
    asked_before_ = std::move(asked_before);
    recorded_ = std::move(recorded);
}

Settlement InputRound::settle(const DeclaredInput& input, InputRequest question) {
    const std::string digest = arguments_digest(question_json(question, context_.control_.profile()));
    Settlement found;
    // An outcome an earlier round recorded for exactly this question.
    if (const auto recorded = recorded_.find(input.key); recorded != recorded_.end() && recorded->second.digest == digest) {
        kept_.insert_or_assign(input.key, recorded->second);
        found.answer = recorded->second.answer;
        return found;
    }
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
        kept_.insert_or_assign(input.key, Recorded{digest, std::string(method_of(question)), answer});
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
    json outcomes = json::object();
    const auto write = [&](const std::string& outcome_key, const Recorded& outcome) {
        json entry = json::object();
        entry[std::string(digest_member)] = outcome.digest;
        entry[std::string(method_member)] = outcome.method;
        entry[std::string(answer_member)] = answer_json(outcome.answer);
        outcomes[outcome_key] = std::move(entry);
    };
    for (const auto& [outcome_key, outcome] : kept_) write(outcome_key, outcome);
    // What an earlier round recorded for an input this entry did not reach (a question that waits for an earlier
    // answer) is kept as it is: it still counts if the question comes out the same.
    for (const auto& [outcome_key, outcome] : recorded_) {
        if (kept_.find(outcome_key) == kept_.end() && asking_.find(outcome_key) == asking_.end()) {
            write(outcome_key, outcome);
        }
    }
    json state = json::object();
    state[std::string(version_member)] = state_version;
    state[std::string(asked_member)] = std::move(asked);
    state[std::string(outcomes_member)] = std::move(outcomes);
    std::string text = serialize(state);
    if (text.size() > max_state_bytes_) {
        throw AnswerRefused("the answers of the call's rounds take " + std::to_string(text.size()) +
                            " bytes to keep, more than the server keeps (" + std::to_string(max_state_bytes_) + ")");
    }
    InputRequired required;
    required.requests = requests_;
    required.state = std::move(text);
    return required;
}

}  // namespace neomcp::detail
