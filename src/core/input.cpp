#include "core/input.h"

#include <algorithm>
#include <array>
#include <span>
#include <type_traits>
#include <utility>

#include "core/wire_names.h"
#include "neomcp/random.h"
#include "neomcp/wire.h"
#include "neomcp/wire_read.h"

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

// The notification that tells a server the client's roots changed, and the flag of the roots capability that
// declares the client sends it.
constexpr std::string_view roots_changed_method = "notifications/roots/list_changed";
constexpr std::string_view list_changed_flag = "listChanged";
// The params type of a sampling request; whether it has tools decides whether a client declares tools.
constexpr std::string_view sampling_params_type = "CreateMessageRequestParams";
// The type a client's capabilities are written for.
constexpr std::string_view client_capabilities_type = "ClientCapabilities";
// The keywords of a form's requested schema that prefilled() reads.
constexpr std::string_view properties_keyword = "properties";
constexpr std::string_view default_keyword = "default";

constexpr std::string_view mode_member = "mode";
constexpr std::string_view form_mode = "form";
constexpr std::string_view url_mode = "url";
constexpr std::string_view elicitation_id_member = "elicitationId";
constexpr std::string_view tools_member = "tools";
constexpr std::string_view tool_choice_member = "toolChoice";

// Members of the params of the requests and of the results that answer them.
constexpr std::string_view message_member = "message";
constexpr std::string_view requested_schema_member = "requestedSchema";
constexpr std::string_view url_member = "url";
constexpr std::string_view messages_member = "messages";
constexpr std::string_view max_tokens_member = "maxTokens";
constexpr std::string_view system_prompt_member = "systemPrompt";
constexpr std::string_view model_preferences_member = "modelPreferences";
constexpr std::string_view hints_member = "hints";
constexpr std::string_view name_member = "name";
constexpr std::string_view cost_priority_member = "costPriority";
constexpr std::string_view speed_priority_member = "speedPriority";
constexpr std::string_view intelligence_priority_member = "intelligencePriority";
constexpr std::string_view temperature_member = "temperature";
constexpr std::string_view stop_sequences_member = "stopSequences";
constexpr std::string_view include_context_member = "includeContext";
constexpr std::string_view metadata_member = "metadata";
constexpr std::string_view role_member = "role";
constexpr std::string_view content_member = "content";
constexpr std::string_view action_member = "action";
constexpr std::string_view model_member = "model";
constexpr std::string_view stop_reason_member = "stopReason";
constexpr std::string_view roots_member = "roots";
constexpr std::string_view uri_member = "uri";
constexpr std::string_view meta_member = "_meta";
constexpr std::string_view type_member = "type";

constexpr std::string_view accept_action = "accept";
constexpr std::string_view decline_action = "decline";
constexpr std::string_view cancel_action = "cancel";

// The root type and the result types of the answers, by name in the wire tables.
constexpr std::string_view root_type = "Root";

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

constexpr std::array<std::pair<IncludeContext, std::string_view>, 3> include_context_names{{
    {IncludeContext::none, "none"},
    {IncludeContext::this_server, "thisServer"},
    {IncludeContext::all_servers, "allServers"},
}};

std::string_view to_wire(IncludeContext context) noexcept {
    for (const auto& [value, name] : include_context_names) {
        if (value == context) return name;
    }
    return {};
}

std::optional<IncludeContext> parse_include_context(std::string_view text) noexcept {
    for (const auto& [value, name] : include_context_names) {
        if (name == text) return value;
    }
    return std::nullopt;
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
    params[std::string(message_member)] = form.message;
    params[std::string(requested_schema_member)] = form.requested_schema;
    return params;
}

Written write_elicitation_url(const ElicitationUrl& url, const profile::Profile& profile, const profile::Type*& type,
                              bool fill_ids) {
    type = profile.type(url_params_type);
    if (type == nullptr) return std::string("the revision has no elicitation in URL mode");
    json params = json::object();
    params[std::string(mode_member)] = std::string(url_mode);
    params[std::string(message_member)] = url.message;
    params[std::string(url_member)] = url.url;
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
        entry[std::string(role_member)] = std::string(wire::role_name(message.role));
        if (blocks.empty()) return std::string("a sampling message has no content");
        if (blocks.size() == 1) {
            entry[std::string(content_member)] = std::move(blocks.front());
        } else if (arrays) {
            entry[std::string(content_member)] = std::move(blocks);
        } else {
            return std::string("the revision takes one content block per sampling message");
        }
        messages.push_back(std::move(entry));
    }
    json params = json::object();
    params[std::string(messages_member)] = std::move(messages);
    params[std::string(max_tokens_member)] = sampling.max_tokens;
    if (sampling.system_prompt) params[std::string(system_prompt_member)] = *sampling.system_prompt;
    if (sampling.model_preferences) {
        const ModelPreferences& preferences = *sampling.model_preferences;
        json written = json::object();
        if (!preferences.hints.empty()) {
            json hints = json::array();
            for (const auto& hint : preferences.hints) {
                json entry = json::object();
                entry[std::string(name_member)] = hint;
                hints.push_back(std::move(entry));
            }
            written[std::string(hints_member)] = std::move(hints);
        }
        if (preferences.cost_priority) written[std::string(cost_priority_member)] = *preferences.cost_priority;
        if (preferences.speed_priority) written[std::string(speed_priority_member)] = *preferences.speed_priority;
        if (preferences.intelligence_priority) written[std::string(intelligence_priority_member)] = *preferences.intelligence_priority;
        params[std::string(model_preferences_member)] = std::move(written);
    }
    if (sampling.temperature) params[std::string(temperature_member)] = *sampling.temperature;
    if (!sampling.stop_sequences.empty()) {
        json sequences = json::array();
        for (const auto& sequence : sampling.stop_sequences) sequences.push_back(sequence);
        params[std::string(stop_sequences_member)] = std::move(sequences);
    }
    if (sampling.include_context) params[std::string(include_context_member)] = std::string(to_wire(*sampling.include_context));
    if (sampling.metadata) params[std::string(metadata_member)] = *sampling.metadata;
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

// A content block as a client writes one: an object with a string `type`.
bool is_block(const json& value) { return value.is_object() && text_member(value, type_member) != nullptr; }

// What is wrong with the content of an elicitation result, or empty when it has the shape a form's answer has: an
// object whose values are strings, numbers, booleans or arrays of strings.
std::optional<std::string> elicitation_content_problem(const json& content) {
    const auto* fields = content.get_ptr<const json::object_t*>();
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
    return std::nullopt;
}

// Whether the value is a content block or a non-empty array of them, as a client writes them in a sampling result.
bool is_block_or_blocks(const json& content) {
    if (const auto* items = content.get_ptr<const json::array_t*>()) {
        bool blocks = !items->empty();
        for (const auto& item : *items) blocks = blocks && is_block(item);
        return blocks;
    }
    return is_block(content);
}

std::variant<InputResponse, std::string> read_elicitation(const json& result) {
    const std::string* action = text_member(result, action_member);
    if (action == nullptr) return std::string("an elicitation result has a string action");
    ElicitationAnswer answer;
    if (*action == accept_action) {
        answer.action = ElicitationAction::accept;
    } else if (*action == decline_action) {
        answer.action = ElicitationAction::decline;
    } else if (*action == cancel_action) {
        answer.action = ElicitationAction::cancel;
    } else {
        return std::string("the action of an elicitation result is accept, decline or cancel");
    }
    if (const json* content = member(result, content_member)) {
        if (auto problem = elicitation_content_problem(*content)) return std::move(*problem);
        // Fields belong to an acceptance; what a client sends with a decline or a cancel is not passed on.
        if (answer.action == ElicitationAction::accept) answer.content = *content;
    }
    return InputResponse(std::move(answer));
}

std::variant<InputResponse, std::string> read_sampling(const json& result) {
    const std::string* role = text_member(result, role_member);
    const std::string* model = text_member(result, model_member);
    const json* content = member(result, content_member);
    const std::optional<Role> parsed_role = role == nullptr ? std::nullopt : wire::parse_role(*role);
    if (!parsed_role) return std::string("the role of a sampling result is user or assistant");
    if (model == nullptr) return std::string("a sampling result names its model");
    if (content == nullptr || !is_block_or_blocks(*content)) return std::string("the content of a sampling result is a content block or an array of them");
    SamplingAnswer answer;
    answer.role = *parsed_role;
    answer.content = *content;
    answer.model = *model;
    if (const json* reason = member(result, stop_reason_member)) {
        const auto* text = reason->get_ptr<const std::string*>();
        if (text == nullptr) return std::string("the stop reason of a sampling result is a string");
        answer.stop_reason = *text;
    }
    return InputResponse(std::move(answer));
}

std::variant<InputResponse, std::string> read_roots(const json& result) {
    const json* roots = member(result, roots_member);
    const auto* items = roots == nullptr ? nullptr : roots->get_ptr<const json::array_t*>();
    if (items == nullptr) return std::string("a roots result has an array of roots");
    RootsAnswer answer;
    for (const auto& item : *items) {
        const std::string* uri = text_member(item, uri_member);
        if (uri == nullptr) return std::string("every root has a string uri");
        Root root;
        root.uri = *uri;
        if (const json* name = member(item, name_member)) {
            const auto* text = name->get_ptr<const std::string*>();
            if (text == nullptr) return std::string("the name of a root is a string");
            root.name = *text;
        }
        if (const json* meta = member(item, meta_member)) {
            if (!meta->is_object()) return std::string("the _meta of a root is an object");
            root.meta = *meta;
        }
        answer.roots.push_back(std::move(root));
    }
    return InputResponse(std::move(answer));
}

// ---- Reading the params of a request

// Each member reader returns what is wrong, or nothing. A member that is absent is left as it is.
std::string read_text_member(const json& object, std::string_view name, std::optional<std::string>& out) {
    const json* found = member(object, name);
    if (found == nullptr) return {};
    const auto* text = found->get_ptr<const std::string*>();
    if (text == nullptr) return "the member " + std::string(name) + " must be a string";
    out = *text;
    return {};
}

std::string read_real_member(const json& object, std::string_view name, std::optional<double>& out) {
    const json* found = member(object, name);
    if (found == nullptr) return {};
    if (!found->is_number()) return "the member " + std::string(name) + " must be a number";
    out = found->get<double>();
    return {};
}

std::string read_object_member(const json& object, std::string_view name, std::optional<json>& out) {
    const json* found = member(object, name);
    if (found == nullptr) return {};
    if (!found->is_object()) return "the member " + std::string(name) + " must be an object";
    out = *found;
    return {};
}

// The members the type requires (where the profile has no type, the ones the mechanism names) that the params lack.
std::optional<std::string> lacking_member(const json& params, const profile::Type* type,
                                          std::span<const std::string> fallback) {
    const std::span<const std::string> required =
        type != nullptr ? std::span<const std::string>(type->required) : fallback;
    for (const auto& name : required) {
        if (member(params, name) == nullptr) {
            return "the request lacks the member " + name + ", which the revision requires";
        }
    }
    return std::nullopt;
}

// Whether the params type has the member; a revision without a type holds nothing back.
bool allows(const profile::Type* type, std::string_view name) { return type == nullptr || type->has_property(name); }

using ReadRequest = std::variant<InputRequest, std::string>;

ReadRequest read_elicitation_request(const json& params, const profile::Profile& profile,
                                     const InputMechanism& mechanism) {
    const profile::Type* form_type = profile.type(form_params_type);
    if (form_type == nullptr) form_type = object_type(profile, mechanism.params_type(profile, elicit_method));
    const profile::Type* url_type = profile.type(url_params_type);
    bool url = false;
    // A request without a mode is a form in every revision. A mode is read where the revision's form params have the
    // member; elsewhere the member is not the revision's and is not read, like any other such member (servers write
    // `mode: "form"` under revisions that have no modes).
    const json* mode = form_type != nullptr && form_type->has_property(mode_member) ? member(params, mode_member) : nullptr;
    if (mode != nullptr) {
        const auto* text = mode->get_ptr<const std::string*>();
        if (text == nullptr) return std::string("the mode of an elicitation request must be a string");
        if (*text == url_mode) {
            if (url_type == nullptr) return std::string("the revision has no elicitation in URL mode");
            url = true;
        } else if (*text != form_mode) {
            return std::string("the mode of an elicitation request is not one the revision defines");
        }
    }
    if (url) {
        if (auto lacking = lacking_member(params, url_type, {})) return std::move(*lacking);
        ElicitationUrl request;
        std::optional<std::string> message;
        std::optional<std::string> address;
        std::string problem = read_text_member(params, message_member, message);
        if (problem.empty()) problem = read_text_member(params, url_member, address);
        if (problem.empty() && url_type->has_property(elicitation_id_member)) {
            problem = read_text_member(params, elicitation_id_member, request.elicitation_id);
        }
        if (!problem.empty()) return problem;
        request.message = message.value_or(std::string());
        request.url = address.value_or(std::string());
        return InputRequest(std::move(request));
    }
    if (auto lacking = lacking_member(params, form_type, mechanism.params_required(profile, elicit_method))) {
        return std::move(*lacking);
    }
    ElicitationForm request;
    std::optional<std::string> message;
    std::optional<json> schema;
    std::string problem = read_text_member(params, message_member, message);
    if (problem.empty()) problem = read_object_member(params, requested_schema_member, schema);
    if (!problem.empty()) return problem;
    request.message = message.value_or(std::string());
    if (schema) request.requested_schema = std::move(*schema);
    return InputRequest(std::move(request));
}

// One block of a sampling message, read for the profile; `where` names it for a message.
std::string read_sampling_block(const json& block_wire, const profile::Profile& profile, const std::string& where,
                                std::vector<ContentBlock>& out) {
    auto block = from_wire<ContentBlock>(block_wire, profile, where);
    if (auto* problem = std::get_if<std::string>(&block)) return std::move(*problem);
    const ContentKind kind = kind_of(std::get<ContentBlock>(block));
    if (kind != ContentKind::text && kind != ContentKind::image && kind != ContentKind::audio) {
        return where + ": a sampling message holds a " + std::string(wire_name(kind)) +
               " block; sampling carries text, image and audio";
    }
    out.push_back(std::move(std::get<ContentBlock>(block)));
    return {};
}

ReadRequest read_sampling_request(const json& params, const profile::Profile& profile,
                                  const InputMechanism& mechanism) {
    const profile::Type* type = object_type(profile, mechanism.params_type(profile, sampling_method));
    if (auto lacking = lacking_member(params, type, mechanism.params_required(profile, sampling_method))) {
        return std::move(*lacking);
    }
    SamplingRequest request;
    const bool arrays = profile.union_type(sampling_blocks_union) != nullptr;
    if (allows(type, messages_member)) {
        if (const json* messages = member(params, messages_member)) {
            const auto* items = messages->get_ptr<const json::array_t*>();
            if (items == nullptr) return std::string("the member messages must be an array");
            for (std::size_t i = 0; i < items->size(); ++i) {
                const json& item = (*items)[i];
                const std::string where = "messages[" + std::to_string(i) + "]";
                if (!item.is_object()) return where + " must be an object";
                const std::string* role_name = text_member(item, role_member);
                const std::optional<Role> role = role_name == nullptr ? std::nullopt : wire::parse_role(*role_name);
                if (!role) return where + ".role must be user or assistant";
                const json* content = member(item, content_member);
                if (content == nullptr) return where + ".content is required";
                SamplingMessage message;
                message.role = *role;
                if (const auto* blocks = content->get_ptr<const json::array_t*>()) {
                    if (!arrays) return where + ".content: the revision takes one content block per sampling message";
                    if (blocks->empty()) return where + ".content must not be empty";
                    for (std::size_t j = 0; j < blocks->size(); ++j) {
                        const std::string problem = read_sampling_block(
                            (*blocks)[j], profile, where + ".content[" + std::to_string(j) + "]", message.content);
                        if (!problem.empty()) return problem;
                    }
                } else {
                    const std::string problem = read_sampling_block(*content, profile, where + ".content", message.content);
                    if (!problem.empty()) return problem;
                }
                request.messages.push_back(std::move(message));
            }
        }
    }
    if (allows(type, max_tokens_member)) {
        if (const json* tokens = member(params, max_tokens_member)) {
            if (const auto* unsigned_tokens = tokens->get_ptr<const json::number_unsigned_t*>()) {
                request.max_tokens = static_cast<std::uint64_t>(*unsigned_tokens);
            } else if (const auto* signed_tokens = tokens->get_ptr<const json::number_integer_t*>();
                       signed_tokens != nullptr && *signed_tokens >= 0) {
                request.max_tokens = static_cast<std::uint64_t>(*signed_tokens);
            } else {
                return std::string("the member maxTokens must be a non-negative integer");
            }
        }
    }
    if (allows(type, system_prompt_member)) {
        if (auto problem = read_text_member(params, system_prompt_member, request.system_prompt); !problem.empty()) {
            return problem;
        }
    }
    if (allows(type, model_preferences_member)) {
        if (const json* found = member(params, model_preferences_member)) {
            if (!found->is_object()) return std::string("the member modelPreferences must be an object");
            ModelPreferences preferences;
            if (const json* hints = member(*found, hints_member)) {
                const auto* items = hints->get_ptr<const json::array_t*>();
                if (items == nullptr) return std::string("the member hints of modelPreferences must be an array");
                for (const auto& hint : *items) {
                    if (!hint.is_object()) return std::string("a hint of modelPreferences must be an object");
                    // A hint may leave out its name: it then says nothing, and there is nothing to keep.
                    const json* hint_name = member(hint, name_member);
                    if (hint_name == nullptr) continue;
                    const auto* text = hint_name->get_ptr<const std::string*>();
                    if (text == nullptr) return std::string("the name of a hint must be a string");
                    preferences.hints.push_back(*text);
                }
            }
            std::string problem = read_real_member(*found, cost_priority_member, preferences.cost_priority);
            if (problem.empty()) problem = read_real_member(*found, speed_priority_member, preferences.speed_priority);
            if (problem.empty()) {
                problem = read_real_member(*found, intelligence_priority_member, preferences.intelligence_priority);
            }
            if (!problem.empty()) return problem;
            request.model_preferences = std::move(preferences);
        }
    }
    if (allows(type, temperature_member)) {
        if (auto problem = read_real_member(params, temperature_member, request.temperature); !problem.empty()) {
            return problem;
        }
    }
    if (allows(type, stop_sequences_member)) {
        if (const json* found = member(params, stop_sequences_member)) {
            const auto* items = found->get_ptr<const json::array_t*>();
            if (items == nullptr) return std::string("the member stopSequences must be an array");
            for (const auto& item : *items) {
                const auto* text = item.get_ptr<const std::string*>();
                if (text == nullptr) return std::string("a stop sequence must be a string");
                request.stop_sequences.push_back(*text);
            }
        }
    }
    if (allows(type, include_context_member)) {
        std::optional<std::string> context;
        if (auto problem = read_text_member(params, include_context_member, context); !problem.empty()) return problem;
        if (context) {
            request.include_context = parse_include_context(*context);
            if (!request.include_context) {
                return std::string("the member includeContext must be none, thisServer or allServers");
            }
        }
    }
    if (allows(type, metadata_member)) {
        if (auto problem = read_object_member(params, metadata_member, request.metadata); !problem.empty()) return problem;
    }
    // Tools are read only where the revision's params have them, the way the writer carries them only there.
    if (type != nullptr && type->has_property(tools_member)) {
        if (const json* found = member(params, tools_member)) {
            const auto* items = found->get_ptr<const json::array_t*>();
            if (items == nullptr) return std::string("the member tools must be an array");
            for (std::size_t i = 0; i < items->size(); ++i) {
                auto tool = from_wire<ToolDefinition>((*items)[i], profile, "tools[" + std::to_string(i) + "]");
                if (auto* problem = std::get_if<std::string>(&tool)) return std::move(*problem);
                request.tools.push_back(std::move(std::get<ToolDefinition>(tool)));
            }
        }
    }
    if (type != nullptr && type->has_property(tool_choice_member)) {
        if (auto problem = read_object_member(params, tool_choice_member, request.tool_choice); !problem.empty()) {
            return problem;
        }
    }
    return InputRequest(std::move(request));
}

// ---- Writing an answer

using WrittenAnswer = std::variant<json, std::string>;

WrittenAnswer write_elicitation_answer(const ElicitationAnswer& answer) {
    json result = json::object();
    std::string_view action = cancel_action;
    switch (answer.action) {
        case ElicitationAction::accept: action = accept_action; break;
        case ElicitationAction::decline: action = decline_action; break;
        case ElicitationAction::cancel: action = cancel_action; break;
    }
    result[std::string(action_member)] = std::string(action);
    // Fields belong to an acceptance, as read_answer takes them.
    if (answer.action == ElicitationAction::accept) {
        if (auto problem = elicitation_content_problem(answer.content)) return std::move(*problem);
        if (!answer.content.empty()) result[std::string(content_member)] = answer.content;
    }
    return result;
}

WrittenAnswer write_sampling_answer(const SamplingAnswer& answer, const profile::Profile& profile) {
    if (!is_block_or_blocks(answer.content)) {
        return std::string("the content of a sampling result is a content block or an array of them");
    }
    // A revision whose sampling messages take one content block (it has no union of sampling blocks) takes one in the
    // result as well: an array of one block is written as that block.
    json content = answer.content;
    if (const auto* blocks = answer.content.get_ptr<const json::array_t*>();
        blocks != nullptr && profile.union_type(sampling_blocks_union) == nullptr) {
        if (blocks->size() != 1) return std::string("the revision takes one content block in a sampling result");
        content = blocks->front();
    }
    json result = json::object();
    result[std::string(role_member)] = std::string(wire::role_name(answer.role));
    result[std::string(content_member)] = std::move(content);
    result[std::string(model_member)] = answer.model;
    if (answer.stop_reason) result[std::string(stop_reason_member)] = *answer.stop_reason;
    return result;
}

WrittenAnswer write_roots_answer(const RootsAnswer& answer, const profile::Profile& profile) {
    json roots = json::array();
    for (const Root& root : answer.roots) {
        json entry = json::object();
        entry[std::string(uri_member)] = root.uri;
        if (root.name) entry[std::string(name_member)] = *root.name;
        if (root.meta) {
            if (!root.meta->is_object()) return std::string("the _meta of a root is an object");
            entry[std::string(meta_member)] = *root.meta;
        }
        keep_members(entry, root_type, profile);
        roots.push_back(std::move(entry));
    }
    json result = json::object();
    result[std::string(roots_member)] = std::move(roots);
    return result;
}

std::string_view result_type_name(std::string_view method) {
    for (const ResultTypeOf& entry : result_types) {
        if (entry.method == method) return entry.type;
    }
    return {};
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

std::variant<InputRequest, std::string> read_request(std::string_view method, const json& params,
                                                     const profile::Profile& profile,
                                                     const InputMechanism& mechanism) {
    if (!mechanism.carries(profile, method)) {
        return "the revision has no " + std::string(method) + " request for a server to ask with";
    }
    if (method == roots_method) {
        // roots/list has no params of its own.
        if (!params.is_null() && !params.is_object()) return std::string("the params of a request must be an object");
        return InputRequest(RootsRequest{});
    }
    if (!params.is_object()) return std::string("the params of a request must be an object");
    if (method == elicit_method) return read_elicitation_request(params, profile, mechanism);
    if (method == sampling_method) return read_sampling_request(params, profile, mechanism);
    return std::string("no request of this kind is asked");
}

std::variant<json, std::string> write_answer(const InputResponse& response, const profile::Profile& profile,
                                             const InputMechanism& mechanism) {
    std::string_view method = roots_method;
    if (std::holds_alternative<ElicitationAnswer>(response)) method = elicit_method;
    if (std::holds_alternative<SamplingAnswer>(response)) method = sampling_method;
    if (!mechanism.carries(profile, method)) {
        return "the revision has no " + std::string(method) + " request for a client to answer";
    }
    WrittenAnswer written = std::visit(
        [&](const auto& value) -> WrittenAnswer {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, ElicitationAnswer>) {
                return write_elicitation_answer(value);
            } else if constexpr (std::is_same_v<T, SamplingAnswer>) {
                return write_sampling_answer(value, profile);
            } else {
                return write_roots_answer(value, profile);
            }
        },
        response);
    auto* result = std::get_if<json>(&written);
    if (result == nullptr) return written;
    if (const profile::Type* type = profile.type(result_type_name(method))) {
        keep_members(*result, type->name, profile);
        for (const auto& name : type->required) {
            if (member(*result, name) == nullptr) {
                return "the answer lacks the member " + name + ", which the revision requires";
            }
        }
    }
    return written;
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

std::optional<std::string_view> roots_changed_notification(const profile::Profile& profile) {
    const profile::Message* entry = profile.notification(roots_changed_method);
    if (entry == nullptr || !entry->from_client) return std::nullopt;
    return roots_changed_method;
}

json declared_capabilities(std::span<const InputRequest> kinds, bool roots_list_changed,
                           const profile::Profile& profile) {
    json capabilities = json::object();
    for (const InputRequest& kind : kinds) {
        const json needed = required_capabilities(kind);
        for (const auto& [name, inner] : needed.get_ref<const json::object_t&>()) {
            json& slot = capabilities[name];
            if (!slot.is_object()) slot = json::object();
            for (const auto& [nested, value] : inner.get_ref<const json::object_t&>()) slot[nested] = value;
        }
    }
    // Elicitation: a mode is declared only where the revision's form params name a mode; elsewhere an empty object
    // declares forms, and a client that takes URLs only declares nothing. URLs only where the revision has them.
    if (json* elicitation = capabilities.contains(std::string(elicitation_capability))
                                ? &capabilities[std::string(elicitation_capability)]
                                : nullptr) {
        const profile::Type* form_type = profile.type(form_params_type);
        const bool modes = form_type != nullptr && form_type->has_property(mode_member);
        const bool form = elicitation->contains(std::string(form_capability));
        if (profile.type(url_params_type) == nullptr) elicitation->erase(std::string(url_capability));
        if (!modes) {
            const bool url = elicitation->contains(std::string(url_capability));
            *elicitation = json::object();
            if (!form && !url) capabilities.erase(std::string(elicitation_capability));
        }
    }
    // Sampling with tools only where the revision's sampling params carry tools.
    if (capabilities.contains(std::string(sampling_capability))) {
        const profile::Type* type = profile.type(sampling_params_type);
        if (type == nullptr || !type->has_property(tools_member)) {
            capabilities[std::string(sampling_capability)].erase(std::string(tools_capability));
        }
    }
    // That the roots change only where the client may send the notification that says so.
    if (roots_list_changed && capabilities.contains(std::string(roots_capability)) &&
        roots_changed_notification(profile)) {
        capabilities[std::string(roots_capability)][std::string(list_changed_flag)] = true;
    }
    keep_members(capabilities, client_capabilities_type, profile);
    return capabilities;
}

std::string_view capability_of(const InputRequest& request) {
    if (std::holds_alternative<SamplingRequest>(request)) return sampling_capability;
    if (std::holds_alternative<RootsRequest>(request)) return roots_capability;
    return elicitation_capability;
}

}  // namespace neomcp::detail

namespace neomcp {

json ElicitationForm::prefilled() const {
    json values = json::object();
    const auto* schema = requested_schema.get_ptr<const json::object_t*>();
    if (schema == nullptr) return values;
    const auto properties = schema->find(std::string(detail::properties_keyword));
    if (properties == schema->end() || !properties->second.is_object()) return values;
    for (const auto& [name, property] : properties->second.get_ref<const json::object_t&>()) {
        const auto* fields = property.get_ptr<const json::object_t*>();
        if (fields == nullptr) continue;
        const auto found = fields->find(std::string(detail::default_keyword));
        if (found != fields->end()) values[name] = found->second;
    }
    return values;
}

}  // namespace neomcp
