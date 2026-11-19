#include "core/input.h"

#include <algorithm>
#include <initializer_list>
#include <span>
#include <type_traits>
#include <utility>

#include "neomcp/random.h"
#include "neomcp/wire.h"

namespace neomcp::detail {
namespace {

// Protocol identifiers; whether a revision has them is read from its profile.
constexpr std::string_view elicit_method = "elicitation/create";
constexpr std::string_view sampling_method = "sampling/createMessage";
constexpr std::string_view roots_method = "roots/list";
// The params types of the two elicitation modes, where a revision names them.
constexpr std::string_view form_params_type = "ElicitRequestFormParams";
constexpr std::string_view url_params_type = "ElicitRequestURLParams";
// The union of the blocks a sampling message holds as an array, where a revision allows an array.
constexpr std::string_view sampling_blocks_union = "SamplingMessageContentBlock";

// The result type of each request a server asks with. The wire tables list the types and what each requires, but do
// not say which request a result type answers: that link is the specification's, and this is it.
struct ResultTypeOf {
    std::string_view method;
    std::string_view type;
};
constexpr ResultTypeOf result_types[] = {
    {elicit_method, "ElicitResult"},
    {sampling_method, "CreateMessageResult"},
    {roots_method, "ListRootsResult"},
};

constexpr std::string_view elicitation_capability = "elicitation";
constexpr std::string_view form_capability = "form";
constexpr std::string_view url_capability = "url";
constexpr std::string_view sampling_capability = "sampling";
constexpr std::string_view tools_capability = "tools";
constexpr std::string_view context_capability = "context";
constexpr std::string_view roots_capability = "roots";

constexpr std::string_view mode_member = "mode";
constexpr std::string_view form_mode = "form";
constexpr std::string_view url_mode = "url";
constexpr std::string_view elicitation_id_member = "elicitationId";
constexpr std::string_view tools_member = "tools";
constexpr std::string_view tool_choice_member = "toolChoice";

// Bytes of a URL elicitation id the session makes.
constexpr std::size_t elicitation_id_bytes = 16;

const json* member(const json& object, std::string_view name) {
    const auto* members = object.get_ptr<const json::object_t*>();
    if (members == nullptr) return nullptr;
    const auto found = members->find(std::string(name));
    return found == members->end() ? nullptr : &found->second;
}

const json* object_member(const json& object, std::string_view name) {
    const json* value = member(object, name);
    return value != nullptr && value->is_object() ? value : nullptr;
}

const std::string* text_member(const json& object, std::string_view name) {
    const json* value = member(object, name);
    return value == nullptr ? nullptr : value->get_ptr<const std::string*>();
}

json capability(std::string_view name, json inner = json::object()) {
    json out = json::object();
    out[std::string(name)] = std::move(inner);
    return out;
}

std::string_view to_wire(IncludeContext context) noexcept {
    switch (context) {
        case IncludeContext::none: return "none";
        case IncludeContext::this_server: return "thisServer";
        case IncludeContext::all_servers: return "allServers";
    }
    return {};
}

bool uses_tools(const SamplingRequest& request) { return !request.tools.empty() || request.tool_choice.has_value(); }

bool uses_context(const SamplingRequest& request) {
    return request.include_context && *request.include_context != IncludeContext::none;
}

// The object type the params of a request are filtered by: the named type of `name`, when it names an object type.
const profile::Type* object_type(const profile::Profile& profile, const std::optional<std::string>& name) {
    return name ? profile.type(*name) : nullptr;
}

using Written = std::variant<json, std::string>;

Written write_elicitation_form(const ElicitationForm& form, const profile::Profile& profile, const InputMechanism& mechanism,
                               const profile::Type*& type) {
    type = profile.type(form_params_type);
    if (type == nullptr) type = object_type(profile, mechanism.params_type(profile, elicit_method));
    json params = json::object();
    // The mode may be left out for a form (every revision reads a request without one as a form); it is written where
    // the revision's params have the member.
    if (type != nullptr && type->has_property(mode_member)) params[std::string(mode_member)] = std::string(form_mode);
    params["message"] = form.message;
    params["requestedSchema"] = form.requested_schema;
    return params;
}

Written write_elicitation_url(const ElicitationUrl& url, const profile::Profile& profile, const profile::Type*& type,
                              bool fill_ids) {
    type = profile.type(url_params_type);
    if (type == nullptr) return std::string("the revision has no elicitation in URL mode");
    json params = json::object();
    params[std::string(mode_member)] = std::string(url_mode);
    params["message"] = url.message;
    params["url"] = url.url;
    if (type->has_property(elicitation_id_member)) {
        if (url.elicitation_id) {
            params[std::string(elicitation_id_member)] = *url.elicitation_id;
        } else if (fill_ids) {
            const auto made = random_hex(elicitation_id_bytes);
            if (!made) return std::string("no elicitation id could be made: the random source failed");
            params[std::string(elicitation_id_member)] = *made;
        } else {
            params[std::string(elicitation_id_member)] = std::string();
        }
    }
    return params;
}

Written write_sampling(const SamplingRequest& sampling, const profile::Profile& profile, const InputMechanism& mechanism,
                       const profile::Type*& type) {
    type = object_type(profile, mechanism.params_type(profile, sampling_method));
    if (uses_tools(sampling) && (type == nullptr || !type->has_property(tools_member) ||
                                 (sampling.tool_choice && !type->has_property(tool_choice_member)))) {
        return std::string("the revision's sampling takes no tools");
    }
    const bool arrays = profile.union_type(sampling_blocks_union) != nullptr;
    json messages = json::array();
    for (const auto& message : sampling.messages) {
        json blocks = json::array();
        for (const auto& block : message.content) {
            const ContentKind kind = kind_of(block);
            if (kind != ContentKind::text && kind != ContentKind::image && kind != ContentKind::audio) {
                return "a sampling message holds a " + std::string(wire_name(kind)) +
                       " block; sampling carries text, image and audio";
            }
            blocks.push_back(neomcp::to_wire(block, profile));
        }
        json entry = json::object();
        entry["role"] = message.role == Role::user ? "user" : "assistant";
        if (blocks.empty()) return std::string("a sampling message has no content");
        if (blocks.size() == 1) {
            entry["content"] = std::move(blocks.front());
        } else if (arrays) {
            entry["content"] = std::move(blocks);
        } else {
            return std::string("the revision takes one content block per sampling message");
        }
        messages.push_back(std::move(entry));
    }
    json params = json::object();
    params["messages"] = std::move(messages);
    params["maxTokens"] = sampling.max_tokens;
    if (sampling.system_prompt) params["systemPrompt"] = *sampling.system_prompt;
    if (sampling.model_preferences) {
        const ModelPreferences& preferences = *sampling.model_preferences;
        json written = json::object();
        if (!preferences.hints.empty()) {
            json hints = json::array();
            for (const auto& hint : preferences.hints) {
                json entry = json::object();
                entry["name"] = hint;
                hints.push_back(std::move(entry));
            }
            written["hints"] = std::move(hints);
        }
        if (preferences.cost_priority) written["costPriority"] = *preferences.cost_priority;
        if (preferences.speed_priority) written["speedPriority"] = *preferences.speed_priority;
        if (preferences.intelligence_priority) written["intelligencePriority"] = *preferences.intelligence_priority;
        params["modelPreferences"] = std::move(written);
    }
    if (sampling.temperature) params["temperature"] = *sampling.temperature;
    if (!sampling.stop_sequences.empty()) {
        json sequences = json::array();
        for (const auto& sequence : sampling.stop_sequences) sequences.push_back(sequence);
        params["stopSequences"] = std::move(sequences);
    }
    if (sampling.include_context) params["includeContext"] = std::string(to_wire(*sampling.include_context));
    if (sampling.metadata) params["metadata"] = *sampling.metadata;
    if (!sampling.tools.empty()) {
        json tools = json::array();
        for (const auto& tool : sampling.tools) tools.push_back(neomcp::to_wire(tool, profile));
        params[std::string(tools_member)] = std::move(tools);
    }
    if (sampling.tool_choice) params[std::string(tool_choice_member)] = *sampling.tool_choice;
    return params;
}

Written write(const InputRequest& request, const profile::Profile& profile, const InputMechanism& mechanism, bool fill_ids) {
    const std::string_view method = method_of(request);
    if (!mechanism.carries(profile, method)) {
        return "the revision has no " + std::string(method) + " request for a server to ask with";
    }
    const profile::Type* type = nullptr;
    Written written = std::visit(
        [&](const auto& value) -> Written {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, ElicitationForm>) {
                return write_elicitation_form(value, profile, mechanism, type);
            } else if constexpr (std::is_same_v<T, ElicitationUrl>) {
                return write_elicitation_url(value, profile, type, fill_ids);
            } else if constexpr (std::is_same_v<T, SamplingRequest>) {
                return write_sampling(value, profile, mechanism, type);
            } else {
                return json(nullptr);  // roots/list has no params of its own
            }
        },
        request);
    auto* params = std::get_if<json>(&written);
    if (params == nullptr || params->is_null()) return written;
    if (type != nullptr) keep_members(*params, type->name, profile);
    const std::span<const std::string> required =
        type != nullptr ? std::span<const std::string>(type->required) : mechanism.params_required(profile, method);
    for (const auto& name : required) {
        if (member(*params, name) == nullptr) {
            return "the request lacks the member " + name + ", which the revision requires";
        }
    }
    return written;
}

bool is_one_of(const std::string& text, std::initializer_list<std::string_view> values) {
    for (const auto value : values) {
        if (text == value) return true;
    }
    return false;
}

// A content block as a client writes one: an object with a string `type`.
bool is_block(const json& value) { return value.is_object() && text_member(value, "type") != nullptr; }

std::variant<InputResponse, std::string> read_elicitation(const json& result) {
    const std::string* action = text_member(result, "action");
    if (action == nullptr) return std::string("an elicitation result has a string action");
    ElicitationAnswer answer;
    if (*action == "accept") {
        answer.action = ElicitationAction::accept;
    } else if (*action == "decline") {
        answer.action = ElicitationAction::decline;
    } else if (*action == "cancel") {
        answer.action = ElicitationAction::cancel;
    } else {
        return std::string("the action of an elicitation result is accept, decline or cancel");
    }
    if (const json* content = member(result, "content")) {
        const auto* fields = content->get_ptr<const json::object_t*>();
        if (fields == nullptr) return std::string("the content of an elicitation result is an object");
        for (const auto& [name, value] : *fields) {
            bool fits = value.is_string() || value.is_number() || value.is_boolean();
            if (const auto* items = value.get_ptr<const json::array_t*>()) {
                fits = true;
                for (const auto& item : *items) fits = fits && item.is_string();
            }
            if (!fits) {
                return "the field " + name +
                       " of an elicitation result is not a string, a number, a boolean or an array of strings";
            }
        }
        // Fields belong to an acceptance; what a client sends with a decline or a cancel is not passed on.
        if (answer.action == ElicitationAction::accept) answer.content = *content;
    }
    return InputResponse(std::move(answer));
}

std::variant<InputResponse, std::string> read_sampling(const json& result) {
    const std::string* role = text_member(result, "role");
    const std::string* model = text_member(result, "model");
    const json* content = member(result, "content");
    if (role == nullptr || !is_one_of(*role, {"user", "assistant"})) {
        return std::string("the role of a sampling result is user or assistant");
    }
    if (model == nullptr) return std::string("a sampling result names its model");
    bool blocks = content != nullptr && is_block(*content);
    if (const auto* items = content == nullptr ? nullptr : content->get_ptr<const json::array_t*>()) {
        blocks = !items->empty();
        for (const auto& item : *items) blocks = blocks && is_block(item);
    }
    if (!blocks) return std::string("the content of a sampling result is a content block or an array of them");
    SamplingAnswer answer;
    answer.role = *role == "user" ? Role::user : Role::assistant;
    answer.content = *content;
    answer.model = *model;
    if (const json* reason = member(result, "stopReason")) {
        const auto* text = reason->get_ptr<const std::string*>();
        if (text == nullptr) return std::string("the stop reason of a sampling result is a string");
        answer.stop_reason = *text;
    }
    return InputResponse(std::move(answer));
}

std::variant<InputResponse, std::string> read_roots(const json& result) {
    const json* roots = member(result, "roots");
    const auto* items = roots == nullptr ? nullptr : roots->get_ptr<const json::array_t*>();
    if (items == nullptr) return std::string("a roots result has an array of roots");
    RootsAnswer answer;
    for (const auto& item : *items) {
        const std::string* uri = text_member(item, "uri");
        if (uri == nullptr) return std::string("every root has a string uri");
        Root root;
        root.uri = *uri;
        if (const json* name = member(item, "name")) {
            const auto* text = name->get_ptr<const std::string*>();
            if (text == nullptr) return std::string("the name of a root is a string");
            root.name = *text;
        }
        if (const json* meta = member(item, "_meta")) {
            if (!meta->is_object()) return std::string("the _meta of a root is an object");
            root.meta = *meta;
        }
        answer.roots.push_back(std::move(root));
    }
    return InputResponse(std::move(answer));
}

}  // namespace

std::string_view method_of(const InputRequest& request) noexcept {
    if (std::holds_alternative<SamplingRequest>(request)) return sampling_method;
    if (std::holds_alternative<RootsRequest>(request)) return roots_method;
    return elicit_method;  // either mode
}

json required_capabilities(const InputRequest& request) {
    return std::visit(
        [](const auto& value) -> json {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, ElicitationForm>) {
                return capability(elicitation_capability, capability(form_capability));
            } else if constexpr (std::is_same_v<T, ElicitationUrl>) {
                return capability(elicitation_capability, capability(url_capability));
            } else if constexpr (std::is_same_v<T, SamplingRequest>) {
                json inner = json::object();
                if (uses_tools(value)) inner[std::string(tools_capability)] = json::object();
                if (uses_context(value)) inner[std::string(context_capability)] = json::object();
                return capability(sampling_capability, std::move(inner));
            } else {
                return capability(roots_capability);
            }
        },
        request);
}

bool capabilities_declare(const InputRequest& request, const json& capabilities) {
    return std::visit(
        [&](const auto& value) -> bool {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, ElicitationForm> || std::is_same_v<T, ElicitationUrl>) {
                const json* elicitation = object_member(capabilities, elicitation_capability);
                if (elicitation == nullptr) return false;
                if constexpr (std::is_same_v<T, ElicitationUrl>) {
                    return object_member(*elicitation, url_capability) != nullptr;
                } else {
                    // An object that names no mode declares forms only.
                    return object_member(*elicitation, form_capability) != nullptr ||
                           (member(*elicitation, form_capability) == nullptr &&
                            member(*elicitation, url_capability) == nullptr);
                }
            } else if constexpr (std::is_same_v<T, SamplingRequest>) {
                const json* sampling = object_member(capabilities, sampling_capability);
                if (sampling == nullptr) return false;
                if (uses_tools(value) && object_member(*sampling, tools_capability) == nullptr) return false;
                if (uses_context(value) && object_member(*sampling, context_capability) == nullptr) return false;
                return true;
            } else {
                return object_member(capabilities, roots_capability) != nullptr;
            }
        },
        request);
}

std::variant<json, std::string> write_params(const InputRequest& request, const profile::Profile& profile,
                                             const InputMechanism& mechanism) {
    return write(request, profile, mechanism, true);
}

bool input_supported(const InputRequest& request, const json& capabilities, const profile::Profile& profile,
                     const InputMechanism& mechanism) {
    return std::holds_alternative<json>(write(request, profile, mechanism, false)) &&
           capabilities_declare(request, capabilities);
}

std::optional<std::string_view> answer_method(const json& result, const profile::Profile& profile,
                                              const InputMechanism& mechanism) {
    if (!result.is_object()) return std::nullopt;
    std::optional<std::string_view> fitting;
    for (const ResultTypeOf& entry : result_types) {
        if (!mechanism.carries(profile, entry.method)) continue;
        const profile::Type* type = profile.type(entry.type);
        if (type == nullptr) continue;
        const bool fits = std::all_of(type->required.begin(), type->required.end(),
                                      [&](const std::string& required) { return member(result, required) != nullptr; });
        if (!fits) continue;
        if (fitting) return std::nullopt;  // the answer of two kinds of request: of none
        fitting = entry.method;
    }
    return fitting;
}

std::variant<InputResponse, std::string> read_answer(std::string_view method, const json& result) {
    if (!result.is_object()) return std::string("an answer is an object");
    if (method == elicit_method) return read_elicitation(result);
    if (method == sampling_method) return read_sampling(result);
    if (method == roots_method) return read_roots(result);
    return std::string("no request of this kind was asked");
}

}  // namespace neomcp::detail
