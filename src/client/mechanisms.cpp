// The built-in client halves of the mechanisms. Each class implements one name that the profile library declares, as
// src/core/mechanisms.cpp does for the server: the protocol identifiers a half works with (methods, members, `_meta`
// keys, type names) are constants of that half, and whatever a revision decides about them (which keys are required,
// which methods exist, which error code refuses a version) is read from the profile.
//
// Where the client reads what the server half also reads (the requests a server asks with, the id a cancellation
// names), the client half asks the server half of the same name, so that the two cannot disagree.
#include "neomcp/client/mechanism.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "core/input.h"
#include "neomcp/wire.h"

namespace neomcp {

namespace {

// ---- reading what a server sent, without throwing

const json* member(const json& object, std::string_view name) {
    const auto* members = object.get_ptr<const json::object_t*>();
    if (members == nullptr) return nullptr;
    const auto found = members->find(std::string(name));
    return found == members->end() ? nullptr : &found->second;
}

// Every member the type requires is in the object; a profile without the type requires nothing.
bool has_required(const json& object, const profile::Type* type) {
    if (type == nullptr) return true;
    return std::all_of(type->required.begin(), type->required.end(),
                       [&](const std::string& name) { return member(object, name) != nullptr; });
}

// A list of strings, or empty when the value is not one.
std::optional<std::vector<std::string>> strings(const json& value) {
    const auto* list = value.get_ptr<const json::array_t*>();
    if (list == nullptr) return std::nullopt;
    std::vector<std::string> out;
    out.reserve(list->size());
    for (const json& item : *list) {
        const auto* text = item.get_ptr<const std::string*>();
        if (text == nullptr) return std::nullopt;
        out.push_back(*text);
    }
    return out;
}

// Whether the member `name` of `object` is the boolean true.
bool flag_set(const json& object, std::string_view name) {
    const json* value = member(object, name);
    return value != nullptr && value->is_boolean() && value->get<bool>();
}

// Whether a server's capabilities object declares `capability` with the boolean member `flag` true.
bool declares(const json& capabilities, std::string_view capability, std::string_view flag) {
    const json* entry = member(capabilities, capability);
    return entry != nullptr && entry->is_object() && flag_set(*entry, flag);
}

// The params of a message the client writes, kept to the members the message's params type has.
json kept(json params, const profile::Message* entry, const profile::Profile& profile) {
    if (entry != nullptr && entry->params) keep_members(params, *entry->params, profile);
    return params;
}

// The members of a JSON-RPC error object.
constexpr std::string_view error_code_member = "code";
constexpr std::string_view error_data_member = "data";

// The `supported` list of an error's data, and the `requested` revision beside it.
std::optional<WireRead<VersionRefusal>> refusal_list(const json& error, std::string_view supported_member,
                                                     std::string_view requested_member) {
    const json* data = member(error, error_data_member);
    const json* supported = data == nullptr ? nullptr : member(*data, supported_member);
    if (supported == nullptr) return std::nullopt;
    auto list = strings(*supported);
    if (!list) return WireRead<VersionRefusal>("$.data." + std::string(supported_member) + ": is not a list of strings");
    VersionRefusal refusal;
    refusal.supported = std::move(*list);
    if (const json* requested = member(*data, requested_member)) {
        const auto* text = requested->get_ptr<const std::string*>();
        if (text == nullptr) return WireRead<VersionRefusal>("$.data." + std::string(requested_member) + ": is not a string");
        refusal.requested = *text;
    }
    return WireRead<VersionRefusal>(std::move(refusal));
}

// The integer code of an error member, when it has one that an int holds (a larger one is no code of the protocol,
// and must not wrap into one).
std::optional<int> error_code_of(const json& error) {
    const json* code = member(error, error_code_member);
    if (code == nullptr || !code->is_number_integer()) return std::nullopt;
    if (code->is_number_unsigned()) {
        const auto value = code->get<std::uint64_t>();
        if (value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) return std::nullopt;
        return static_cast<int>(value);
    }
    const auto value = code->get<std::int64_t>();
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) return std::nullopt;
    return static_cast<int>(value);
}

// ---- open

// `initialize`: a handshake opens the connection; the requests after it carry nothing of their own.
class InitializeOpen final : public ClientOpenMechanism {
public:
    static constexpr std::string_view name = "initialize";
    static constexpr std::string_view method = "initialize";
    static constexpr std::string_view ready_method = "notifications/initialized";
    // The lifecycle lets a client send pings before the handshake has been answered.
    static constexpr std::string_view before_open = "ping";
    static constexpr std::string_view result_type = "InitializeResult";
    static constexpr std::string_view version_member = "protocolVersion";
    static constexpr std::string_view capabilities_member = "capabilities";
    static constexpr std::string_view client_info_member = "clientInfo";
    static constexpr std::string_view server_info_member = "serverInfo";
    static constexpr std::string_view instructions_member = "instructions";
    static constexpr std::string_view set_level_method = "logging/setLevel";
    static constexpr std::string_view level_member = "level";
    static constexpr std::string_view logging_capability = "logging";
    // The members of the data of an Invalid params answer that refuses the requested version.
    static constexpr std::string_view supported_member = "supported";
    static constexpr std::string_view requested_member = "requested";

    std::optional<std::string> check(const profile::Profile& profile) const override {
        const profile::Message* request = profile.request(method);
        if (request == nullptr || !request->from_client) return "the profile has no client request " + std::string(method);
        const profile::Message* ready = profile.notification(ready_method);
        if (ready == nullptr || !ready->from_client) {
            return "the profile has no client notification " + std::string(ready_method);
        }
        // The params this half writes; a revision that requires another one cannot be opened by it.
        for (const auto& required : request->params_required) {
            if (required != version_member && required != capabilities_member && required != client_info_member) {
                return "the profile's " + std::string(method) + " requires the member " + required +
                       ", which this mechanism does not write";
            }
        }
        return std::nullopt;
    }

    bool handshakes() const override { return true; }
    bool names_revision_per_request() const override { return false; }

    std::optional<std::string> annotate(json& params, const profile::Profile&, const ClientSelf&) const override {
        if (!params.is_null() && !params.is_object()) return std::string("the params of a request must be an object");
        return std::nullopt;
    }

    std::optional<ClientMessage> handshake(const profile::Profile& profile, const ClientSelf& self) const override {
        const profile::Message* entry = profile.request(method);
        if (entry == nullptr) return std::nullopt;
        json params = json::object();
        params[std::string(version_member)] = profile.revision();
        params[std::string(capabilities_member)] = self.capabilities.is_object() ? self.capabilities : json::object();
        params[std::string(client_info_member)] = to_wire(self.info, profile);
        return ClientMessage{std::string(method), kept(std::move(params), entry, profile)};
    }

    bool allowed_before_open(std::string_view name_of) const override { return name_of == before_open; }

    std::variant<HandshakeAccepted, UnservedRevision, std::string> read_handshake(
        const json& result, std::span<const profile::ProfilePtr> candidates) const override {
        if (!result.is_object()) return std::string("$: is not an object");
        const json* version = member(result, version_member);
        const auto* named = version == nullptr ? nullptr : version->get_ptr<const std::string*>();
        if (named == nullptr) return "$." + std::string(version_member) + ": is required and must be a string";
        profile::ProfilePtr chosen;
        for (const auto& candidate : candidates) {
            if (candidate != nullptr && candidate->revision() == *named) chosen = candidate;
        }
        if (chosen == nullptr) return UnservedRevision{*named};
        const profile::Type* type = chosen->type(result_type);
        if (!has_required(result, type)) return "$: lacks a member the revision's " + std::string(result_type) + " requires";
        HandshakeAccepted accepted;
        accepted.profile = chosen;
        if (auto problem = announcement(result, accepted.server)) return std::move(*problem);
        return accepted;
    }

    std::optional<ClientMessage> opened(const profile::Profile& profile) const override {
        if (profile.notification(ready_method) == nullptr) return std::nullopt;
        return ClientMessage{std::string(ready_method), nullptr};
    }

    std::optional<ClientMessage> probe(const profile::Profile&) const override { return std::nullopt; }
    std::optional<WireRead<Discovery>> read_discovery(const json&, const profile::Profile&) const override {
        return std::nullopt;
    }

    std::optional<WireRead<VersionRefusal>> read_version_refusal(const json& error,
                                                                 const profile::Profile&) const override {
        if (error_code_of(error) != error_code::invalid_params) return std::nullopt;
        return refusal_list(error, supported_member, requested_member);
    }

    std::optional<ClientMessage> level_request(LogLevel level, const profile::Profile& profile,
                                               const json& server_capabilities) const override {
        const profile::Message* entry = profile.request(set_level_method);
        if (entry == nullptr || !entry->from_client) return std::nullopt;
        const json* logging = member(server_capabilities, logging_capability);
        if (logging == nullptr || !logging->is_object()) return std::nullopt;
        json params = json::object();
        params[std::string(level_member)] = std::string(to_string(level));
        return ClientMessage{std::string(set_level_method), kept(std::move(params), entry, profile)};
    }

private:
    static std::optional<std::string> announcement(const json& result, ServerAnnouncement& out) {
        if (const json* capabilities = member(result, capabilities_member)) {
            if (!capabilities->is_object()) return "$." + std::string(capabilities_member) + ": is not an object";
            out.capabilities = *capabilities;
        }
        if (const json* info = member(result, server_info_member)) {
            if (!info->is_object()) return "$." + std::string(server_info_member) + ": is not an object";
            out.info = *info;
        }
        if (const json* instructions = member(result, instructions_member)) {
            const auto* text = instructions->get_ptr<const std::string*>();
            if (text == nullptr) return "$." + std::string(instructions_member) + ": is not a string";
            out.instructions = *text;
        }
        out.result = result;
        return std::nullopt;
    }
};

// `request_meta`: every request names its revision and its client in params._meta.
class RequestMetaOpen final : public ClientOpenMechanism {
public:
    static constexpr std::string_view name = "request_meta";
    static constexpr std::string_view meta_type = "RequestMetaObject";
    static constexpr std::string_view meta_member = "_meta";
    static constexpr std::string_view version_key = "io.modelcontextprotocol/protocolVersion";
    static constexpr std::string_view capabilities_key = "io.modelcontextprotocol/clientCapabilities";
    static constexpr std::string_view client_info_key = "io.modelcontextprotocol/clientInfo";
    static constexpr std::string_view log_level_key = "io.modelcontextprotocol/logLevel";
    static constexpr std::string_view discover_method = "server/discover";
    static constexpr std::string_view discover_result_type = "DiscoverResult";
    static constexpr std::string_view versions_member = "supportedVersions";
    static constexpr std::string_view capabilities_member = "capabilities";
    static constexpr std::string_view instructions_member = "instructions";
    static constexpr std::string_view server_info_key = "io.modelcontextprotocol/serverInfo";
    // The name of the error code, in the profile's `errors`, that refuses the version a request names.
    static constexpr std::string_view unsupported_version_error = "unsupported_protocol_version";
    static constexpr std::string_view supported_member = "supported";
    static constexpr std::string_view requested_member = "requested";

    std::optional<std::string> check(const profile::Profile& profile) const override {
        const profile::Type* type = profile.type(meta_type);
        if (type == nullptr) return "the profile has no type " + std::string(meta_type);
        if (!type->is_required(version_key)) {
            return "the profile's " + std::string(meta_type) + " does not require " + std::string(version_key);
        }
        // The keys this half fills; the log level only when one is set, so a revision cannot require it.
        for (const auto& key : type->required) {
            if (key != version_key && key != capabilities_key && key != client_info_key) {
                return "the profile's " + std::string(meta_type) + " requires the key " + key +
                       ", which this mechanism does not write on every request";
            }
        }
        const profile::Message* discover = profile.request(discover_method);
        if (discover == nullptr || !discover->from_client) {
            return "the profile has no client request " + std::string(discover_method);
        }
        if (profile.type(discover_result_type) == nullptr) {
            return "the profile has no type " + std::string(discover_result_type);
        }
        return std::nullopt;
    }

    bool handshakes() const override { return false; }
    bool names_revision_per_request() const override { return true; }

    std::optional<std::string> annotate(json& params, const profile::Profile& profile,
                                        const ClientSelf& self) const override {
        if (params.is_null()) params = json::object();
        if (!params.is_object()) return std::string("the params of a request must be an object");
        const profile::Type* type = profile.type(meta_type);
        if (type == nullptr) return "the profile has no type " + std::string(meta_type);
        json& meta = params[std::string(meta_member)];
        if (meta.is_null()) meta = json::object();
        if (!meta.is_object()) return "params." + std::string(meta_member) + " must be an object";
        for (const auto key : {version_key, capabilities_key, client_info_key, log_level_key}) meta.erase(std::string(key));
        meta[std::string(version_key)] = profile.revision();
        if (type->has_property(capabilities_key)) {
            meta[std::string(capabilities_key)] = self.capabilities.is_object() ? self.capabilities : json::object();
        }
        if (type->has_property(client_info_key)) meta[std::string(client_info_key)] = to_wire(self.info, profile);
        if (self.log_level && type->has_property(log_level_key)) {
            meta[std::string(log_level_key)] = std::string(to_string(*self.log_level));
        }
        return std::nullopt;
    }

    std::optional<ClientMessage> handshake(const profile::Profile&, const ClientSelf&) const override {
        return std::nullopt;
    }
    bool allowed_before_open(std::string_view) const override { return false; }
    std::variant<HandshakeAccepted, UnservedRevision, std::string> read_handshake(
        const json&, std::span<const profile::ProfilePtr>) const override {
        return std::string("this mechanism opens no connection with a handshake");
    }
    std::optional<ClientMessage> opened(const profile::Profile&) const override { return std::nullopt; }

    std::optional<ClientMessage> probe(const profile::Profile& profile) const override {
        if (profile.request(discover_method) == nullptr) return std::nullopt;
        return ClientMessage{std::string(discover_method), json::object()};
    }

    std::optional<WireRead<Discovery>> read_discovery(const json& result,
                                                      const profile::Profile& profile) const override {
        const profile::Type* type = profile.type(discover_result_type);
        if (type == nullptr || !result.is_object() || !has_required(result, type)) return std::nullopt;
        Discovery discovery;
        const json* versions = member(result, versions_member);
        auto list = versions == nullptr ? std::nullopt : strings(*versions);
        if (!list) return WireRead<Discovery>("$." + std::string(versions_member) + ": is not a list of strings");
        discovery.versions = std::move(*list);
        ServerAnnouncement& server = discovery.server;
        if (const json* capabilities = member(result, capabilities_member)) {
            if (!capabilities->is_object()) {
                return WireRead<Discovery>("$." + std::string(capabilities_member) + ": is not an object");
            }
            server.capabilities = *capabilities;
        }
        if (const json* instructions = member(result, instructions_member)) {
            const auto* text = instructions->get_ptr<const std::string*>();
            if (text == nullptr) return WireRead<Discovery>("$." + std::string(instructions_member) + ": is not a string");
            server.instructions = *text;
        }
        if (const json* meta = member(result, meta_member)) {
            if (!meta->is_object()) return WireRead<Discovery>("$." + std::string(meta_member) + ": is not an object");
            if (const json* info = member(*meta, server_info_key)) {
                if (!info->is_object()) {
                    return WireRead<Discovery>("$." + std::string(meta_member) + "[\"" + std::string(server_info_key) +
                                               "\"]: is not an object");
                }
                server.info = *info;
            }
        }
        server.result = result;
        return WireRead<Discovery>(std::move(discovery));
    }

    std::optional<WireRead<VersionRefusal>> read_version_refusal(const json& error,
                                                                 const profile::Profile& profile) const override {
        const auto code = profile.error_code(unsupported_version_error);
        if (!code || error_code_of(error) != *code) return std::nullopt;
        auto read = refusal_list(error, supported_member, requested_member);
        // The code itself refuses the version: an answer of it without a list is malformed.
        if (!read) return WireRead<VersionRefusal>("$.data." + std::string(supported_member) + ": is required");
        return read;
    }

    std::optional<ClientMessage> level_request(LogLevel, const profile::Profile&, const json&) const override {
        return std::nullopt;
    }
};

// ---- state

class ConnectionState final : public ClientStateMechanism {
public:
    static constexpr std::string_view name = "connection";
    bool keeps_connection() const override { return true; }
};

class NoState final : public ClientStateMechanism {
public:
    static constexpr std::string_view name = "none";
    bool keeps_connection() const override { return false; }
};

// ---- batch

// `array`: a server may send an array of messages. The client answers each request of it with a response of its own:
// a body that is a single response is allowed beside an array of responses, and answering one by one lets the answer
// of a quick handler go before that of a slow one.
class ArrayBatch final : public ClientBatchMechanism {
public:
    static constexpr std::string_view name = "array";
    bool reads_arrays() const override { return true; }
    WireRead<std::vector<json>> split(json value) const override {
        std::vector<json> messages;
        if (auto* list = value.get_ptr<json::array_t*>()) {
            if (list->empty()) return std::string("$: is an empty array");
            messages.reserve(list->size());
            for (auto& item : *list) messages.push_back(std::move(item));
            return messages;
        }
        messages.push_back(std::move(value));
        return messages;
    }
};

class NoBatch final : public ClientBatchMechanism {
public:
    static constexpr std::string_view name = "none";
    bool reads_arrays() const override { return false; }
    WireRead<std::vector<json>> split(json value) const override {
        if (value.is_array()) return std::string("$: is an array, which the revision does not allow");
        std::vector<json> messages;
        messages.push_back(std::move(value));
        return messages;
    }
};

// ---- cancel

// The server half of the same name, which reads what a server's cancellation names exactly as a server reads a
// client's.
std::unique_ptr<CancelMechanism> server_cancel(std::string_view name) {
    auto half = builtin_mechanisms().make_cancel(name);
    if (half == nullptr) throw std::logic_error("the library has no server half of the cancel mechanism " + std::string(name));
    return half;
}

// `cancelled_notification`: the client sends notifications/cancelled with the request's id.
class CancelledNotification final : public ClientCancelMechanism {
public:
    static constexpr std::string_view name = "cancelled_notification";
    static constexpr std::string_view method = "notifications/cancelled";
    static constexpr std::string_view request_id_member = "requestId";
    static constexpr std::string_view reason_member = "reason";

    CancelledNotification() : server_(server_cancel(name)) {}

    std::optional<std::string> check(const profile::Profile& profile) const override {
        const profile::Message* entry = profile.notification(method);
        if (entry == nullptr || !entry->from_client) return "the profile has no client notification " + std::string(method);
        return std::nullopt;
    }
    bool stream_close_cancels() const override { return false; }
    std::optional<ClientMessage> notification(const RequestId& id, std::string_view reason,
                                              const profile::Profile& profile) const override {
        const profile::Message* entry = profile.notification(method);
        if (entry == nullptr) return std::nullopt;
        json params = json::object();
        params[std::string(request_id_member)] = id.to_json();
        if (!reason.empty()) params[std::string(reason_member)] = std::string(reason);
        return ClientMessage{std::string(method), kept(std::move(params), entry, profile)};
    }
    std::optional<RequestId> cancelled_request(const Message& notification) const override {
        return server_->cancelled_request(notification);
    }

private:
    std::unique_ptr<CancelMechanism> server_;
};

// `close_response_stream`: ending the stream that carries a request's answer cancels it; nothing is sent.
class CloseResponseStream final : public ClientCancelMechanism {
public:
    static constexpr std::string_view name = "close_response_stream";

    CloseResponseStream() : server_(server_cancel(name)) {}

    std::optional<std::string> check(const profile::Profile&) const override { return std::nullopt; }
    bool stream_close_cancels() const override { return true; }
    std::optional<ClientMessage> notification(const RequestId&, std::string_view,
                                              const profile::Profile&) const override {
        return std::nullopt;
    }
    std::optional<RequestId> cancelled_request(const Message& notification) const override {
        return server_->cancelled_request(notification);
    }

private:
    std::unique_ptr<CancelMechanism> server_;
};

// ---- server_input

std::unique_ptr<InputMechanism> server_input(std::string_view name) {
    auto half = builtin_mechanisms().make_input(name);
    if (half == nullptr) {
        throw std::logic_error("the library has no server half of the server_input mechanism " + std::string(name));
    }
    return half;
}

// Shared by both halves: the notification that tells of new roots, where the profile lets a client send it.
std::optional<ClientMessage> roots_notification(const profile::Profile& profile) {
    const auto method = detail::roots_changed_notification(profile);
    if (!method) return std::nullopt;
    return ClientMessage{std::string(*method), nullptr};
}

// ---- the restricted form of a form's requested schema, shared by both halves

// The members the root of a requested schema may have: what a form is (its type and its properties), which of its
// properties are required, and the dialect it is written in.
constexpr std::string_view form_type_member = "type";
constexpr std::string_view form_properties_member = "properties";
constexpr std::string_view form_required_member = "required";
constexpr std::string_view form_dialect_member = "$schema";
constexpr std::string_view form_root_members[] = {form_type_member, form_properties_member, form_required_member,
                                                  form_dialect_member};
// The union of the schemas one property of a form may have, in the profile's wire tables.
constexpr std::string_view primitive_schema_union = "PrimitiveSchemaDefinition";

// The object types a union resolves to: its alternatives, through unions that name unions. `depth` bounds a table
// that names itself.
void union_members(const profile::Profile& profile, std::string_view name, std::vector<const profile::Type*>& out,
                   int depth) {
    if (depth <= 0) return;
    if (const profile::Type* type = profile.type(name)) {
        out.push_back(type);
        return;
    }
    if (const profile::Union* alternatives = profile.union_type(name)) {
        for (const auto& alternative : alternatives->alternatives) union_members(profile, alternative, out, depth - 1);
    }
}

// Whether a property schema has every member the type requires and no member the type lacks.
bool fits(const json::object_t& property, const profile::Type& type) {
    for (const auto& [keyword, unused] : property) {
        static_cast<void>(unused);
        if (!type.has_property(keyword)) return false;
    }
    return std::all_of(type.required.begin(), type.required.end(),
                       [&](const std::string& name) { return property.find(name) != property.end(); });
}

// The tables name the members of each primitive schema definition but not the values of their members, so this check
// compares member names only: a property whose `type` names something other than its alternative's passes here
// (the engine compiles the schema too, and the accepted content is validated against it).
std::optional<std::string> restricted_form_problem(const json& schema, const profile::Profile& profile) {
    constexpr int union_depth = 8;
    const auto* root = schema.get_ptr<const json::object_t*>();
    if (root == nullptr) return std::string("the requested schema is not an object");
    for (const auto& [keyword, unused] : *root) {
        static_cast<void>(unused);
        if (std::find(std::begin(form_root_members), std::end(form_root_members), keyword) ==
            std::end(form_root_members)) {
            return std::string("the requested schema has a member a form does not define at its root");
        }
    }
    if (root->find(std::string(form_type_member)) == root->end()) {
        return std::string("the requested schema has no type");
    }
    const auto properties = root->find(std::string(form_properties_member));
    if (properties == root->end() || !properties->second.is_object()) {
        return std::string("the requested schema has no object of properties");
    }
    std::vector<const profile::Type*> alternatives;
    union_members(profile, primitive_schema_union, alternatives, union_depth);
    if (alternatives.empty()) return std::string("the revision defines no form");
    std::size_t position = 0;
    for (const auto& [name, property] : properties->second.get_ref<const json::object_t&>()) {
        static_cast<void>(name);
        const auto* fields = property.get_ptr<const json::object_t*>();
        const bool fitting =
            fields != nullptr && std::any_of(alternatives.begin(), alternatives.end(),
                                             [&](const profile::Type* alternative) { return fits(*fields, *alternative); });
        if (!fitting) {
            return "property " + std::to_string(position + 1) +
                   " of the requested schema fits none of the revision's primitive schema definitions";
        }
        ++position;
    }
    return std::nullopt;
}

RequestError not_applicable(std::string_view what) {
    return RequestError{error_code::internal_error, std::string(what) + " does not apply to this mechanism"};
}

// `input_required_result`: the call's result asks; the client retries the call with the answers and the state.
class InputRequiredResult final : public ClientInputMechanism {
public:
    static constexpr std::string_view name = "input_required_result";
    static constexpr std::string_view responses_member = "inputResponses";
    static constexpr std::string_view state_member = "requestState";
    static constexpr std::string_view requests_member = "inputRequests";
    static constexpr std::string_view result_type_member = "resultType";
    static constexpr std::string_view input_required = "input_required";
    static constexpr std::string_view complete = "complete";
    static constexpr std::string_view method_member = "method";
    static constexpr std::string_view params_member = "params";

    InputRequiredResult() : server_(server_input(name)) {}

    std::optional<std::string> check(const profile::Profile& profile) const override { return server_->check(profile); }
    InputDelivery delivery() const override { return InputDelivery::result; }
    bool asks_on(const profile::Profile& profile, std::string_view method) const override {
        return server_->asks_on(profile, method);
    }

    WireRead<std::optional<InputAsking>> read_result(const json& result, std::string_view method,
                                                     const profile::Profile& profile) const override {
        using Read = WireRead<std::optional<InputAsking>>;
        if (!result.is_object()) return Read(std::string("$: is not an object"));
        const json* kind = member(result, result_type_member);
        if (kind == nullptr) return Read(std::optional<InputAsking>());  // a result that says nothing is complete
        const auto* text = kind->get_ptr<const std::string*>();
        if (text == nullptr) return Read("$." + std::string(result_type_member) + ": is not a string");
        if (*text == complete) return Read(std::optional<InputAsking>());
        if (*text != input_required) return Read("$." + std::string(result_type_member) + ": is not a kind this client knows");
        if (!asks_on(profile, method)) return Read(std::string("$: asks for input on a request that cannot ask"));

        InputAsking asking;
        if (const json* requests = member(result, requests_member)) {
            const auto* entries = requests->get_ptr<const json::object_t*>();
            if (entries == nullptr) return Read("$." + std::string(requests_member) + ": is not an object");
            for (const auto& [key, entry] : *entries) {
                const std::string at = "$." + std::string(requests_member) + "[" + std::to_string(asking.requests.size()) + "]";
                const json* asked = member(entry, method_member);
                const auto* asked_method = asked == nullptr ? nullptr : asked->get_ptr<const std::string*>();
                if (asked_method == nullptr) return Read(at + "." + std::string(method_member) + ": is required and must be a string");
                if (!server_->carries(profile, *asked_method)) {
                    return Read(at + "." + std::string(method_member) + ": is no request the revision lets a server ask with");
                }
                const json* params = member(entry, params_member);
                auto request = detail::read_request(*asked_method, params == nullptr ? json() : *params, profile, *server_);
                if (auto* problem = std::get_if<std::string>(&request)) return Read(at + ": " + *problem);
                asking.requests.push_back(AskedInput{key, *asked_method, std::move(std::get<InputRequest>(request))});
            }
        }
        if (const json* state = member(result, state_member)) asking.state = *state;
        if (asking.requests.empty() && !asking.state) {
            return Read(std::string("$: asks with neither requests nor a state"));
        }
        std::sort(asking.requests.begin(), asking.requests.end(),
                  [](const AskedInput& a, const AskedInput& b) { return a.key < b.key; });
        return Read(std::optional<InputAsking>(std::move(asking)));
    }

    WireRead<json> retry_params(json params, const InputResponses& answers, const std::optional<json>& state,
                                const profile::Profile& profile) const override {
        if (params.is_null()) params = json::object();
        if (!params.is_object()) return std::string("the params of a request must be an object");
        params.erase(std::string(responses_member));
        params.erase(std::string(state_member));
        if (!answers.empty()) {
            json written = json::object();
            for (const auto& [key, answer] : answers) {
                auto one = detail::write_answer(answer, profile, *server_);
                if (auto* problem = std::get_if<std::string>(&one)) return std::move(*problem);
                written[key] = std::move(std::get<json>(one));
            }
            params[std::string(responses_member)] = std::move(written);
        }
        if (state) {
            // The state goes back exactly as it came: a text that serializing would change (not well-formed UTF-8)
            // cannot, and an altered state is never sent.
            const json echoed = json::parse(serialize(*state), nullptr, false);
            if (echoed.is_discarded() || echoed != *state) {
                return std::string("the request state cannot be sent back exactly as it was received");
            }
            params[std::string(state_member)] = *state;
        }
        return params;
    }

    std::variant<ServerAsk, ServerPing, RequestError> read_request(const Message&,
                                                                   const profile::Profile&) const override {
        return RequestError{error_code::method_not_found, "Method not found"};
    }
    RequestError refusal(const InputRequest&, const json&, const profile::Profile&) const override {
        return not_applicable("an answer to a server's request");
    }
    RequestError user_refusal(const profile::Profile&) const override {
        return not_applicable("an answer to a server's request");
    }
    WireRead<json> answer(const RequestId&, const InputResponse&, const profile::Profile&) const override {
        return std::string("under this mechanism the client answers no request of the server");
    }
    json ping_answer(const RequestId& id) const override { return make_result(id, json::object()); }

    std::optional<std::string> form_problem(const json& requested_schema,
                                            const profile::Profile& profile) const override {
        return restricted_form_problem(requested_schema, profile);
    }

    std::optional<ClientMessage> roots_changed(const profile::Profile& profile) const override {
        return roots_notification(profile);
    }

private:
    std::unique_ptr<InputMechanism> server_;
};

// `server_request`: the server sends its requests to the client while a call is pending, and the client answers each.
class ServerRequest final : public ClientInputMechanism {
public:
    static constexpr std::string_view name = "server_request";
    static constexpr std::string_view ping_method = "ping";

    ServerRequest() : server_(server_input(name)) {}

    std::optional<std::string> check(const profile::Profile& profile) const override { return server_->check(profile); }
    InputDelivery delivery() const override { return InputDelivery::requests; }
    bool asks_on(const profile::Profile& profile, std::string_view method) const override {
        return server_->asks_on(profile, method);
    }

    WireRead<std::optional<InputAsking>> read_result(const json&, std::string_view,
                                                     const profile::Profile&) const override {
        return std::optional<InputAsking>();
    }
    WireRead<json> retry_params(json, const InputResponses&, const std::optional<json>&,
                                const profile::Profile&) const override {
        return std::string("under this mechanism no call is retried with answers");
    }

    std::variant<ServerAsk, ServerPing, RequestError> read_request(const Message& request,
                                                                   const profile::Profile& profile) const override {
        if (request.kind != MessageKind::request || !request.id) {
            return RequestError{error_code::invalid_request, "Invalid Request"};
        }
        if (request.method == ping_method) {
            const profile::Message* entry = profile.request(ping_method);
            if (entry != nullptr && entry->from_server) return ServerPing{};
        }
        if (!server_->carries(profile, request.method)) return RequestError{error_code::method_not_found, "Method not found"};
        auto read = detail::read_request(request.method, request.params, profile, *server_);
        if (auto* problem = std::get_if<std::string>(&read)) {
            return RequestError{error_code::invalid_params, "Invalid params: " + *problem};
        }
        return ServerAsk{request.method, std::move(std::get<InputRequest>(read))};
    }

    // The error a user's refusal of a sampling request is answered with (the sampling page's code for it).
    static constexpr int user_rejected_code = -1;
    static constexpr std::string_view user_rejected_message = "User rejected sampling request";

    // A kind of request the client declares nothing of (no elicitation, no sampling, no roots) is answered as a method
    // the client does not have. A request of a kind it declares that asks for something it did not declare within
    // that kind (a mode of elicitation, sampling with tools, sampling with context) is answered with Invalid params.
    RequestError refusal(const InputRequest& request, const json& declared, const profile::Profile&) const override {
        if (member(declared, detail::capability_of(request)) == nullptr) {
            return RequestError{error_code::method_not_found, "Method not found"};
        }
        return RequestError{error_code::invalid_params,
                            "Invalid params: the client does not support what this request asks for"};
    }
    RequestError user_refusal(const profile::Profile&) const override {
        return RequestError{user_rejected_code, std::string(user_rejected_message)};
    }

    WireRead<json> answer(const RequestId& id, const InputResponse& response,
                          const profile::Profile& profile) const override {
        auto written = detail::write_answer(response, profile, *server_);
        if (auto* problem = std::get_if<std::string>(&written)) return std::move(*problem);
        return make_result(id, std::move(std::get<json>(written)));
    }
    json ping_answer(const RequestId& id) const override { return make_result(id, json::object()); }

    std::optional<std::string> form_problem(const json& requested_schema,
                                            const profile::Profile& profile) const override {
        return restricted_form_problem(requested_schema, profile);
    }

    std::optional<ClientMessage> roots_changed(const profile::Profile& profile) const override {
        return roots_notification(profile);
    }

private:
    std::unique_ptr<InputMechanism> server_;
};

// ---- change_feed

std::unique_ptr<ChangeFeedMechanism> server_feed(std::string_view name) {
    auto half = builtin_mechanisms().make_feed(name);
    if (half == nullptr) {
        throw std::logic_error("the library has no server half of the change_feed mechanism " + std::string(name));
    }
    return half;
}

// What both halves read about a change: the notifications every revision names the same way.
class ChangeNotifications {
public:
    static constexpr std::string_view tools_changed = "notifications/tools/list_changed";
    static constexpr std::string_view prompts_changed = "notifications/prompts/list_changed";
    static constexpr std::string_view resources_changed = "notifications/resources/list_changed";
    static constexpr std::string_view resource_updated = "notifications/resources/updated";
    static constexpr std::string_view uri_member = "uri";

    // The change a notification of the profile tells of; empty when it tells of none.
    static std::optional<WireRead<Change>> read(const Message& notification, const profile::Profile& profile) {
        const profile::Message* entry = profile.notification(notification.method);
        if (entry == nullptr || !entry->from_server) return std::nullopt;
        Change change;
        if (notification.method == tools_changed) {
            change.kind = ChangeKind::tools_list;
        } else if (notification.method == prompts_changed) {
            change.kind = ChangeKind::prompts_list;
        } else if (notification.method == resources_changed) {
            change.kind = ChangeKind::resources_list;
        } else if (notification.method == resource_updated) {
            change.kind = ChangeKind::resource_updated;
            const json* uri = member(notification.params, uri_member);
            const auto* text = uri == nullptr ? nullptr : uri->get_ptr<const std::string*>();
            if (text == nullptr) return WireRead<Change>("$.params." + std::string(uri_member) + ": is required and must be a string");
            change.uri = *text;
        } else {
            return std::nullopt;
        }
        return WireRead<Change>(std::move(change));
    }
};

// `connection_notifications`: the changes come as notifications of the connection; resources/subscribe and
// resources/unsubscribe add and remove a resource the connection watches.
class ConnectionNotifications final : public ClientFeedMechanism {
public:
    static constexpr std::string_view name = "connection_notifications";
    static constexpr std::string_view subscribe_method = "resources/subscribe";
    static constexpr std::string_view unsubscribe_method = "resources/unsubscribe";
    static constexpr std::string_view uri_member = "uri";
    static constexpr std::string_view tools_capability = "tools";
    static constexpr std::string_view prompts_capability = "prompts";
    static constexpr std::string_view resources_capability = "resources";
    static constexpr std::string_view list_changed_flag = "listChanged";
    static constexpr std::string_view subscribe_flag = "subscribe";

    ConnectionNotifications() : server_(server_feed(name)) {}

    std::optional<std::string> check(const profile::Profile& profile) const override { return server_->check(profile); }
    FeedDelivery delivery() const override { return FeedDelivery::connection; }

    ChangeFilter offered(const ChangeFilter& asked, const json& server_capabilities) const override {
        ChangeFilter offered;
        offered.tools_list = asked.tools_list && declares(server_capabilities, tools_capability, list_changed_flag);
        offered.prompts_list = asked.prompts_list && declares(server_capabilities, prompts_capability, list_changed_flag);
        offered.resources_list =
            asked.resources_list && declares(server_capabilities, resources_capability, list_changed_flag);
        if (declares(server_capabilities, resources_capability, subscribe_flag)) offered.resources = asked.resources;
        return offered;
    }

    WireRead<ClientMessage> write(const FeedRequest& asked, const profile::Profile& profile) const override {
        std::string_view method;
        switch (asked.action) {
            case FeedRequest::Action::watch: method = subscribe_method; break;
            case FeedRequest::Action::unwatch: method = unsubscribe_method; break;
            case FeedRequest::Action::listen: return std::string("this mechanism opens no subscription");
        }
        const profile::Message* entry = profile.request(method);
        if (entry == nullptr || !entry->from_client) return "the profile has no client request " + std::string(method);
        json params = json::object();
        params[std::string(uri_member)] = asked.uri;
        return ClientMessage{std::string(method), kept(std::move(params), entry, profile)};
    }

    std::optional<WireRead<FeedEvent>> read(const Message& notification,
                                            const profile::Profile& profile) const override {
        auto change = ChangeNotifications::read(notification, profile);
        if (!change) return std::nullopt;
        if (auto* problem = std::get_if<std::string>(&*change)) return WireRead<FeedEvent>(std::move(*problem));
        FeedEvent event;
        event.kind = FeedEvent::Kind::change;
        event.change = std::move(std::get<Change>(*change));
        return WireRead<FeedEvent>(std::move(event));
    }

    std::optional<std::string> read_end(const json&, const RequestId&, const profile::Profile&) const override {
        return std::string("under this mechanism no result ends a subscription");
    }

private:
    std::unique_ptr<ChangeFeedMechanism> server_;
};

// `subscriptions_listen`: subscriptions/listen opens a subscription whose answer is a stream. Its first message is the
// acknowledgement of what of the filter is honoured; every message of it carries the listen request's id in `_meta`;
// the server ends it with notifications/cancelled naming that request, and then a result.
class SubscriptionsListen final : public ClientFeedMechanism {
public:
    static constexpr std::string_view name = "subscriptions_listen";
    static constexpr std::string_view listen_method = "subscriptions/listen";
    static constexpr std::string_view acknowledged_method = "notifications/subscriptions/acknowledged";
    static constexpr std::string_view cancelled_method = "notifications/cancelled";
    static constexpr std::string_view request_id_member = "requestId";
    static constexpr std::string_view meta_member = "_meta";
    static constexpr std::string_view subscription_key = "io.modelcontextprotocol/subscriptionId";
    static constexpr std::string_view filter_member = "notifications";
    static constexpr std::string_view tools_member = "toolsListChanged";
    static constexpr std::string_view prompts_member = "promptsListChanged";
    static constexpr std::string_view resources_member = "resourcesListChanged";
    static constexpr std::string_view updates_member = "resourceSubscriptions";
    static constexpr std::string_view result_type = "SubscriptionsListenResult";
    static constexpr std::string_view result_meta_type = "SubscriptionsListenResultMetaObject";
    static constexpr std::string_view result_type_member = "resultType";
    static constexpr std::string_view complete = "complete";

    SubscriptionsListen() : server_(server_feed(name)) {}

    std::optional<std::string> check(const profile::Profile& profile) const override { return server_->check(profile); }
    FeedDelivery delivery() const override { return FeedDelivery::subscription; }

    ChangeFilter offered(const ChangeFilter& asked, const json&) const override { return asked; }

    WireRead<ClientMessage> write(const FeedRequest& asked, const profile::Profile& profile) const override {
        if (asked.action != FeedRequest::Action::listen) {
            return std::string("this mechanism watches no single resource; a subscription names them");
        }
        const profile::Message* entry = profile.request(listen_method);
        if (entry == nullptr || !entry->from_client) return "the profile has no client request " + std::string(listen_method);
        json filter = json::object();
        if (asked.filter.tools_list) filter[std::string(tools_member)] = true;
        if (asked.filter.prompts_list) filter[std::string(prompts_member)] = true;
        if (asked.filter.resources_list) filter[std::string(resources_member)] = true;
        if (!asked.filter.resources.empty()) {
            json uris = json::array();
            for (const auto& uri : asked.filter.resources) uris.push_back(uri);
            filter[std::string(updates_member)] = std::move(uris);
        }
        json params = json::object();
        params[std::string(filter_member)] = std::move(filter);
        return ClientMessage{std::string(listen_method), kept(std::move(params), entry, profile)};
    }

    std::optional<WireRead<FeedEvent>> read(const Message& notification,
                                            const profile::Profile& profile) const override {
        using Read = WireRead<FeedEvent>;
        FeedEvent event;
        if (notification.method == cancelled_method) {
            const json* id = member(notification.params, request_id_member);
            auto named = id == nullptr ? std::nullopt : RequestId::from_json(*id);
            if (!named) return Read("$.params." + std::string(request_id_member) + ": is required and must be a string or an integer");
            event.kind = FeedEvent::Kind::ended;
            event.subscription = std::move(*named);
            return Read(std::move(event));
        }
        if (notification.method == acknowledged_method) {
            event.kind = FeedEvent::Kind::acknowledged;
            const json* filter = member(notification.params, filter_member);
            if (filter == nullptr || !filter->is_object()) {
                return Read("$.params." + std::string(filter_member) + ": is required and must be an object");
            }
            if (auto problem = read_filter(*filter, event.honoured)) return Read(std::move(*problem));
        } else {
            auto change = ChangeNotifications::read(notification, profile);
            if (!change) return std::nullopt;
            if (auto* problem = std::get_if<std::string>(&*change)) return Read(std::move(*problem));
            event.kind = FeedEvent::Kind::change;
            event.change = std::move(std::get<Change>(*change));
        }
        // Every message of a subscription names the subscription it belongs to.
        const json* meta = member(notification.params, meta_member);
        const json* tag = meta == nullptr ? nullptr : member(*meta, subscription_key);
        auto subscription = tag == nullptr ? std::nullopt : RequestId::from_json(*tag);
        if (!subscription) {
            return Read("$.params." + std::string(meta_member) + "[\"" + std::string(subscription_key) +
                        "\"]: is required and must be a string or an integer");
        }
        event.subscription = std::move(*subscription);
        return Read(std::move(event));
    }

    std::optional<std::string> read_end(const json& result, const RequestId& subscription,
                                        const profile::Profile& profile) const override {
        if (!result.is_object()) return std::string("$: is not an object");
        if (!has_required(result, profile.type(result_type))) {
            return "$: lacks a member the revision's " + std::string(result_type) + " requires";
        }
        if (const json* kind = member(result, result_type_member)) {
            const auto* text = kind->get_ptr<const std::string*>();
            if (text == nullptr || *text != complete) return "$." + std::string(result_type_member) + ": is not complete";
        }
        if (const json* meta = member(result, meta_member)) {
            if (!meta->is_object()) return "$." + std::string(meta_member) + ": is not an object";
            if (!has_required(*meta, profile.type(result_meta_type))) {
                return "$." + std::string(meta_member) + ": lacks a key the revision's " + std::string(result_meta_type) +
                       " requires";
            }
            const json* tag = member(*meta, subscription_key);
            if (tag != nullptr) {
                auto named = RequestId::from_json(*tag);
                if (!named || !(*named == subscription)) {
                    return "$." + std::string(meta_member) + "[\"" + std::string(subscription_key) +
                           "\"]: does not name the subscription";
                }
            }
        }
        return std::nullopt;
    }

private:
    // The members of a filter as the server acknowledged it.
    static std::optional<std::string> read_filter(const json& filter, ChangeFilter& out) {
        const std::string at = "$.params." + std::string(filter_member) + ".";
        const auto flag = [&](std::string_view name_of, bool& value) -> std::optional<std::string> {
            const json* given = member(filter, name_of);
            if (given == nullptr) return std::nullopt;
            if (!given->is_boolean()) return at + std::string(name_of) + ": is not a boolean";
            value = given->get<bool>();
            return std::nullopt;
        };
        if (auto problem = flag(tools_member, out.tools_list)) return problem;
        if (auto problem = flag(prompts_member, out.prompts_list)) return problem;
        if (auto problem = flag(resources_member, out.resources_list)) return problem;
        if (const json* uris = member(filter, updates_member)) {
            auto list = strings(*uris);
            if (!list) return at + std::string(updates_member) + ": is not a list of strings";
            for (auto& uri : *list) {
                if (std::find(out.resources.begin(), out.resources.end(), uri) == out.resources.end()) {
                    out.resources.push_back(std::move(uri));
                }
            }
        }
        return std::nullopt;
    }

    std::unique_ptr<ChangeFeedMechanism> server_;
};

// ---- the built-in registry

template <typename Interface, typename Implementation>
void add(ClientMechanismRegistry& registry, void (ClientMechanismRegistry::*adder)(std::string,
                                                                                   ClientMechanismRegistry::Factory<Interface>)) {
    (registry.*adder)(std::string(Implementation::name),
                      [] { return std::unique_ptr<Interface>(std::make_unique<Implementation>()); });
}

ClientMechanismRegistry make_builtin() {
    ClientMechanismRegistry registry;
    add<ClientOpenMechanism, InitializeOpen>(registry, &ClientMechanismRegistry::add_open);
    add<ClientOpenMechanism, RequestMetaOpen>(registry, &ClientMechanismRegistry::add_open);
    add<ClientStateMechanism, ConnectionState>(registry, &ClientMechanismRegistry::add_state);
    add<ClientStateMechanism, NoState>(registry, &ClientMechanismRegistry::add_state);
    add<ClientBatchMechanism, ArrayBatch>(registry, &ClientMechanismRegistry::add_batch);
    add<ClientBatchMechanism, NoBatch>(registry, &ClientMechanismRegistry::add_batch);
    add<ClientCancelMechanism, CancelledNotification>(registry, &ClientMechanismRegistry::add_cancel);
    add<ClientCancelMechanism, CloseResponseStream>(registry, &ClientMechanismRegistry::add_cancel);
    add<ClientInputMechanism, InputRequiredResult>(registry, &ClientMechanismRegistry::add_input);
    add<ClientInputMechanism, ServerRequest>(registry, &ClientMechanismRegistry::add_input);
    add<ClientFeedMechanism, ConnectionNotifications>(registry, &ClientMechanismRegistry::add_feed);
    add<ClientFeedMechanism, SubscriptionsListen>(registry, &ClientMechanismRegistry::add_feed);
    return registry;
}

MechanismSlot slot_of(profile::Slot slot) {
    switch (slot) {
        case profile::Slot::open: return MechanismSlot::open;
        case profile::Slot::state: return MechanismSlot::state;
        case profile::Slot::server_input: return MechanismSlot::server_input;
        case profile::Slot::change_feed: return MechanismSlot::change_feed;
        case profile::Slot::batch: return MechanismSlot::batch;
    }
    return MechanismSlot::open;
}

// The name of the cancellation mechanism the profile's entry for the transport names; empty when the profile does
// not define the transport.
std::optional<std::string_view> cancel_name(const profile::Profile& profile, profile::Transport transport) {
    switch (transport) {
        case profile::Transport::stdio:
            if (profile.stdio()) return std::string_view(profile.stdio()->cancel);
            return std::nullopt;
        case profile::Transport::http_sse:
            if (profile.http_sse()) return std::string_view(profile.http_sse()->cancel);
            return std::nullopt;
        case profile::Transport::streamable_http:
            if (profile.streamable_http()) return std::string_view(profile.streamable_http()->cancel);
            return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace

template <typename Interface>
void ClientMechanismRegistry::put(MechanismSlot slot, std::string name, Factory<Interface> factory) {
    factories_.insert_or_assign(Key(slot, std::move(name)),
                                AnyFactory(std::in_place_type<Factory<Interface>>, std::move(factory)));
}

template <typename Interface>
std::unique_ptr<Interface> ClientMechanismRegistry::make(MechanismSlot slot, std::string_view name) const {
    const auto found = factories_.find(Key(slot, std::string(name)));
    if (found == factories_.end()) return nullptr;
    const auto* make_one = std::get_if<Factory<Interface>>(&found->second);
    if (make_one == nullptr || !*make_one) return nullptr;
    return (*make_one)();
}

void ClientMechanismRegistry::add_open(std::string name, Factory<ClientOpenMechanism> factory) {
    put(MechanismSlot::open, std::move(name), std::move(factory));
}
void ClientMechanismRegistry::add_state(std::string name, Factory<ClientStateMechanism> factory) {
    put(MechanismSlot::state, std::move(name), std::move(factory));
}
void ClientMechanismRegistry::add_batch(std::string name, Factory<ClientBatchMechanism> factory) {
    put(MechanismSlot::batch, std::move(name), std::move(factory));
}
void ClientMechanismRegistry::add_cancel(std::string name, Factory<ClientCancelMechanism> factory) {
    put(MechanismSlot::cancel, std::move(name), std::move(factory));
}
void ClientMechanismRegistry::add_input(std::string name, Factory<ClientInputMechanism> factory) {
    put(MechanismSlot::server_input, std::move(name), std::move(factory));
}
void ClientMechanismRegistry::add_feed(std::string name, Factory<ClientFeedMechanism> factory) {
    put(MechanismSlot::change_feed, std::move(name), std::move(factory));
}

bool ClientMechanismRegistry::remove(MechanismSlot slot, std::string_view name) {
    return factories_.erase(Key(slot, std::string(name))) > 0;
}

bool ClientMechanismRegistry::contains(MechanismSlot slot, std::string_view name) const {
    return factories_.find(Key(slot, std::string(name))) != factories_.end();
}

std::vector<std::string> ClientMechanismRegistry::names(MechanismSlot slot) const {
    std::vector<std::string> out;
    for (const auto& entry : factories_) {
        if (entry.first.first == slot) out.push_back(entry.first.second);
    }
    return out;
}

std::unique_ptr<ClientOpenMechanism> ClientMechanismRegistry::make_open(std::string_view name) const {
    return make<ClientOpenMechanism>(MechanismSlot::open, name);
}
std::unique_ptr<ClientStateMechanism> ClientMechanismRegistry::make_state(std::string_view name) const {
    return make<ClientStateMechanism>(MechanismSlot::state, name);
}
std::unique_ptr<ClientBatchMechanism> ClientMechanismRegistry::make_batch(std::string_view name) const {
    return make<ClientBatchMechanism>(MechanismSlot::batch, name);
}
std::unique_ptr<ClientCancelMechanism> ClientMechanismRegistry::make_cancel(std::string_view name) const {
    return make<ClientCancelMechanism>(MechanismSlot::cancel, name);
}
std::unique_ptr<ClientInputMechanism> ClientMechanismRegistry::make_input(std::string_view name) const {
    return make<ClientInputMechanism>(MechanismSlot::server_input, name);
}
std::unique_ptr<ClientFeedMechanism> ClientMechanismRegistry::make_feed(std::string_view name) const {
    return make<ClientFeedMechanism>(MechanismSlot::change_feed, name);
}

const ClientMechanismRegistry& builtin_client_mechanisms() {
    static const ClientMechanismRegistry registry = make_builtin();
    return registry;
}

std::variant<std::vector<ClientMechanisms>, ClientMechanismsError> select_client_mechanisms(
    std::span<const profile::ProfilePtr> profiles, profile::Transport transport,
    const ClientMechanismRegistry& registry) {
    ClientMechanismsError error;
    const auto problem = [&](std::string line) {
        if (!error.message.empty()) error.message += '\n';
        error.message += line;
    };
    const auto missing = [&](const std::string& revision, MechanismSlot slot, std::string_view name) {
        problem("revision " + revision + ": no client half of the " + std::string(to_string(slot)) + " mechanism '" +
                std::string(name) + "'");
        error.missing.push_back(MissingClientMechanism{revision, slot, std::string(name)});
    };
    const auto unable = [&](const std::string& revision, MechanismSlot slot, std::string_view name,
                            const std::optional<std::string>& why) {
        if (!why) return;
        problem("revision " + revision + ": the client half of the " + std::string(to_string(slot)) + " mechanism '" +
                std::string(name) + "' cannot speak it: " + *why);
    };

    std::vector<ClientMechanisms> selected;
    for (const auto& profile : profiles) {
        if (profile == nullptr) {
            problem("a profile is null");
            continue;
        }
        const auto cancel = cancel_name(*profile, transport);
        if (!cancel) continue;
        const std::string& revision = profile->revision();
        ClientMechanisms halves;
        halves.profile = profile;

        const std::string_view open_name = profile->mechanism(profile::Slot::open);
        halves.open = registry.make_open(open_name);
        if (halves.open == nullptr) {
            missing(revision, MechanismSlot::open, open_name);
        } else {
            unable(revision, MechanismSlot::open, open_name, halves.open->check(*profile));
        }
        const std::string_view state_name = profile->mechanism(profile::Slot::state);
        halves.state = registry.make_state(state_name);
        if (halves.state == nullptr) missing(revision, slot_of(profile::Slot::state), state_name);
        const std::string_view batch_name = profile->mechanism(profile::Slot::batch);
        halves.batch = registry.make_batch(batch_name);
        if (halves.batch == nullptr) missing(revision, slot_of(profile::Slot::batch), batch_name);
        halves.cancel = registry.make_cancel(*cancel);
        if (halves.cancel == nullptr) {
            missing(revision, MechanismSlot::cancel, *cancel);
        } else {
            unable(revision, MechanismSlot::cancel, *cancel, halves.cancel->check(*profile));
        }
        const std::string_view input_name = profile->mechanism(profile::Slot::server_input);
        halves.server_input = registry.make_input(input_name);
        if (halves.server_input == nullptr) {
            missing(revision, MechanismSlot::server_input, input_name);
        } else {
            unable(revision, MechanismSlot::server_input, input_name, halves.server_input->check(*profile));
        }
        const std::string_view feed_name = profile->mechanism(profile::Slot::change_feed);
        halves.change_feed = registry.make_feed(feed_name);
        if (halves.change_feed == nullptr) {
            missing(revision, MechanismSlot::change_feed, feed_name);
        } else {
            unable(revision, MechanismSlot::change_feed, feed_name, halves.change_feed->check(*profile));
        }
        selected.push_back(std::move(halves));
    }
    if (!error.message.empty()) return error;
    return selected;
}

}  // namespace neomcp
