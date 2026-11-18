// The built-in mechanisms. Each class implements one name that the profile library declares; the protocol
// identifiers a mechanism works with (methods, `_meta` keys, type names) are constants of that mechanism, and
// whatever a revision decides about them (which keys are required, which methods exist) is read from the profile.
#include "neomcp/mechanism.h"

#include <utility>

namespace neomcp {

std::string_view to_string(MechanismSlot slot) noexcept {
    switch (slot) {
        case MechanismSlot::open: return "open";
        case MechanismSlot::state: return "state";
        case MechanismSlot::batch: return "batch";
        case MechanismSlot::cancel: return "cancel";
    }
    return {};
}

namespace {

const json* member(const json& object, std::string_view name) {
    const auto* members = object.get_ptr<const json::object_t*>();
    if (members == nullptr) return nullptr;
    const auto found = members->find(std::string(name));
    return found == members->end() ? nullptr : &found->second;
}

RequestError invalid_params(std::string message) { return RequestError{error_code::invalid_params, std::move(message)}; }

RequestError not_applicable(std::string_view what) {
    return RequestError{error_code::internal_error, std::string(what) + " does not apply to this mechanism"};
}

// ---- open

// `initialize`: a handshake opens the connection; requests after it are served under the negotiated revision.
class InitializeOpen final : public OpenMechanism {
public:
    static constexpr std::string_view method = "initialize";
    // The lifecycle lets a client send pings before the handshake has been answered.
    static constexpr std::string_view before_open = "ping";

    std::optional<std::string> check(const profile::Profile& profile) const override {
        const profile::Message* request = profile.request(method);
        if (request == nullptr || !request->from_client) {
            return "the profile has no client request " + std::string(method);
        }
        return std::nullopt;
    }

    bool needs_connection_state() const override { return true; }
    bool requests_name_revision() const override { return false; }

    std::optional<std::variant<std::string, RequestError>> named_revision(const Message&) const override {
        return std::nullopt;
    }

    std::variant<RequestScope, RequestError> scope(const Message&, const profile::Profile&, bool) const override {
        return not_applicable("a per-request scope");
    }

    bool opens_connection(const Message& request) const override { return request.method == method; }

    std::variant<Handshake, RequestError> open(const Message& request,
                                               std::span<const profile::ProfilePtr> candidates) const override {
        if (!request.params.is_object()) return invalid_params("initialize needs params");
        const json* version = member(request.params, "protocolVersion");
        const auto* requested = version == nullptr ? nullptr : version->get_ptr<const std::string*>();
        if (requested == nullptr) return invalid_params("initialize: params.protocolVersion must be a string");

        // The requested revision when it is served, else the newest one served.
        profile::ProfilePtr chosen = candidates.front();
        for (const auto& candidate : candidates) {
            if (candidate->revision() == *requested) chosen = candidate;
        }

        if (const profile::Message* entry = chosen->request(method)) {
            for (const auto& name : entry->params_required) {
                if (member(request.params, name) == nullptr) {
                    return invalid_params("initialize: params lacks the required member " + name);
                }
            }
        }
        Handshake handshake;
        handshake.profile = chosen;
        if (const json* info = member(request.params, "clientInfo")) {
            if (!info->is_object()) return invalid_params("initialize: params.clientInfo must be an object");
            handshake.client.info = *info;
        }
        if (const json* capabilities = member(request.params, "capabilities")) {
            if (!capabilities->is_object()) return invalid_params("initialize: params.capabilities must be an object");
            handshake.client.capabilities = *capabilities;
        }
        return handshake;
    }

    bool allowed_before_open(std::string_view name) const override { return name == before_open; }

    std::string missing_signal() const override { return "no initialize request opened this connection"; }
};

// `request_meta`: every request names its revision and its client in params._meta.
class RequestMetaOpen final : public OpenMechanism {
public:
    static constexpr std::string_view meta_type = "RequestMetaObject";
    static constexpr std::string_view version_key = "io.modelcontextprotocol/protocolVersion";
    static constexpr std::string_view capabilities_key = "io.modelcontextprotocol/clientCapabilities";
    static constexpr std::string_view client_info_key = "io.modelcontextprotocol/clientInfo";
    static constexpr std::string_view log_level_key = "io.modelcontextprotocol/logLevel";

    std::optional<std::string> check(const profile::Profile& profile) const override {
        const profile::Type* type = profile.type(meta_type);
        if (type == nullptr) return "the profile has no type " + std::string(meta_type);
        if (!type->is_required(version_key)) {
            return "the profile's " + std::string(meta_type) + " does not require " + std::string(version_key);
        }
        return std::nullopt;
    }

    bool needs_connection_state() const override { return false; }
    bool requests_name_revision() const override { return true; }

    std::optional<std::variant<std::string, RequestError>> named_revision(const Message& request) const override {
        const json* meta = member(request.params, "_meta");
        const json* version = meta == nullptr ? nullptr : member(*meta, version_key);
        if (version == nullptr) return std::nullopt;
        const auto* text = version->get_ptr<const std::string*>();
        if (text == nullptr) return invalid_params("params._meta[\"" + std::string(version_key) + "\"] must be a string");
        return *text;
    }

    std::variant<RequestScope, RequestError> scope(const Message& request, const profile::Profile& profile,
                                                   bool protocol_logging) const override {
        const profile::Type* type = profile.type(meta_type);
        const json* meta = member(request.params, "_meta");
        if (type == nullptr || meta == nullptr) return not_applicable("a request without " + std::string(meta_type));
        for (const auto& key : type->required) {
            if (member(*meta, key) == nullptr) return invalid_params("params._meta lacks the required key " + key);
        }
        RequestScope scope;
        if (const json* capabilities = member(*meta, capabilities_key)) {
            if (!capabilities->is_object()) {
                return invalid_params("params._meta[\"" + std::string(capabilities_key) + "\"] must be an object");
            }
            scope.client.capabilities = *capabilities;
        }
        if (const json* info = member(*meta, client_info_key)) {
            if (!info->is_object()) {
                return invalid_params("params._meta[\"" + std::string(client_info_key) + "\"] must be an object");
            }
            scope.client.info = *info;
        }
        if (protocol_logging && type->has_property(log_level_key)) {
            if (const json* level = member(*meta, log_level_key)) {
                const auto* text = level->get_ptr<const std::string*>();
                const auto parsed = text == nullptr ? std::nullopt : parse_log_level(*text);
                if (!parsed) {
                    return invalid_params("params._meta[\"" + std::string(log_level_key) + "\"] is not a log level");
                }
                scope.log_level = parsed;
            }
        }
        return scope;
    }

    bool opens_connection(const Message&) const override { return false; }

    std::variant<Handshake, RequestError> open(const Message&, std::span<const profile::ProfilePtr>) const override {
        return not_applicable("a handshake");
    }

    bool allowed_before_open(std::string_view) const override { return false; }

    std::string missing_signal() const override {
        return "params._meta has no \"" + std::string(version_key) + "\"";
    }
};

// ---- state

class ConnectionState final : public StateMechanism {
public:
    bool keeps_connection() const override { return true; }
};

class NoState final : public StateMechanism {
public:
    bool keeps_connection() const override { return false; }
};

// ---- batch

class ArrayBatch final : public BatchMechanism {
public:
    bool accepts_arrays() const override { return true; }
    std::optional<json> assemble(std::vector<json> responses) const override {
        if (responses.empty()) return std::nullopt;
        json array = json::array();
        for (auto& response : responses) array.push_back(std::move(response));
        return array;
    }
};

class NoBatch final : public BatchMechanism {
public:
    bool accepts_arrays() const override { return false; }
    std::optional<json> assemble(std::vector<json>) const override { return std::nullopt; }
};

// ---- cancel

// `cancelled_notification`: the client sends notifications/cancelled with the request's id.
class CancelledNotification final : public CancelMechanism {
public:
    static constexpr std::string_view method = "notifications/cancelled";

    std::optional<RequestId> cancelled_request(const Message& notification) const override {
        if (notification.method != method) return std::nullopt;
        const json* id = member(notification.params, "requestId");
        if (id == nullptr) return std::nullopt;
        return RequestId::from_json(*id);
    }

    // A client that only lost the stream of the answer may still want it: disconnection is not a cancellation.
    bool stops_on_stream_close() const override { return false; }
};

// `close_response_stream`: a request is cancelled by closing the stream that carries its response, an event of the
// HTTP transport rather than a message; no notification cancels anything under it.
class CloseResponseStream final : public CancelMechanism {
public:
    std::optional<RequestId> cancelled_request(const Message&) const override { return std::nullopt; }
    bool stops_on_stream_close() const override { return true; }
};

template <typename Interface, typename Implementation>
MechanismRegistry::Factory<Interface> factory() {
    return [] { return std::unique_ptr<Interface>(std::make_unique<Implementation>()); };
}

MechanismRegistry make_builtin() {
    MechanismRegistry registry;
    registry.add_open("initialize", factory<OpenMechanism, InitializeOpen>());
    registry.add_open("request_meta", factory<OpenMechanism, RequestMetaOpen>());
    registry.add_state("connection", factory<StateMechanism, ConnectionState>());
    registry.add_state("none", factory<StateMechanism, NoState>());
    registry.add_batch("array", factory<BatchMechanism, ArrayBatch>());
    registry.add_batch("none", factory<BatchMechanism, NoBatch>());
    registry.add_cancel("cancelled_notification", factory<CancelMechanism, CancelledNotification>());
    registry.add_cancel("close_response_stream", factory<CancelMechanism, CloseResponseStream>());
    return registry;
}

}  // namespace

template <typename Interface>
void MechanismRegistry::put(MechanismSlot slot, std::string name, Factory<Interface> factory) {
    factories_.insert_or_assign(Key(slot, std::move(name)),
                                AnyFactory(std::in_place_type<Factory<Interface>>, std::move(factory)));
}

template <typename Interface>
std::unique_ptr<Interface> MechanismRegistry::make(MechanismSlot slot, std::string_view name) const {
    const auto found = factories_.find(Key(slot, std::string(name)));
    if (found == factories_.end()) return nullptr;
    const auto* make_one = std::get_if<Factory<Interface>>(&found->second);
    if (make_one == nullptr || !*make_one) return nullptr;
    return (*make_one)();
}

void MechanismRegistry::add_open(std::string name, Factory<OpenMechanism> factory) {
    put(MechanismSlot::open, std::move(name), std::move(factory));
}
void MechanismRegistry::add_state(std::string name, Factory<StateMechanism> factory) {
    put(MechanismSlot::state, std::move(name), std::move(factory));
}
void MechanismRegistry::add_batch(std::string name, Factory<BatchMechanism> factory) {
    put(MechanismSlot::batch, std::move(name), std::move(factory));
}
void MechanismRegistry::add_cancel(std::string name, Factory<CancelMechanism> factory) {
    put(MechanismSlot::cancel, std::move(name), std::move(factory));
}

bool MechanismRegistry::remove(MechanismSlot slot, std::string_view name) {
    return factories_.erase(Key(slot, std::string(name))) > 0;
}

bool MechanismRegistry::contains(MechanismSlot slot, std::string_view name) const {
    return factories_.find(Key(slot, std::string(name))) != factories_.end();
}

std::vector<std::string> MechanismRegistry::names(MechanismSlot slot) const {
    std::vector<std::string> out;
    for (const auto& entry : factories_) {
        if (entry.first.first == slot) out.push_back(entry.first.second);
    }
    return out;
}

std::unique_ptr<OpenMechanism> MechanismRegistry::make_open(std::string_view name) const {
    return make<OpenMechanism>(MechanismSlot::open, name);
}
std::unique_ptr<StateMechanism> MechanismRegistry::make_state(std::string_view name) const {
    return make<StateMechanism>(MechanismSlot::state, name);
}
std::unique_ptr<BatchMechanism> MechanismRegistry::make_batch(std::string_view name) const {
    return make<BatchMechanism>(MechanismSlot::batch, name);
}
std::unique_ptr<CancelMechanism> MechanismRegistry::make_cancel(std::string_view name) const {
    return make<CancelMechanism>(MechanismSlot::cancel, name);
}

const MechanismRegistry& builtin_mechanisms() {
    static const MechanismRegistry registry = make_builtin();
    return registry;
}

}  // namespace neomcp
