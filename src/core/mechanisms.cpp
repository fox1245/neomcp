// The built-in mechanisms. Each class implements one name that the profile library declares; the protocol
// identifiers a mechanism works with (methods, `_meta` keys, type names) are constants of that mechanism, and
// whatever a revision decides about them (which keys are required, which methods exist) is read from the profile.
#include "neomcp/mechanism.h"

#include <algorithm>
#include <utility>

#include "core/tasks.h"

namespace neomcp {

std::string_view to_string(MechanismSlot slot) noexcept {
    switch (slot) {
        case MechanismSlot::open: return "open";
        case MechanismSlot::state: return "state";
        case MechanismSlot::batch: return "batch";
        case MechanismSlot::cancel: return "cancel";
        case MechanismSlot::server_input: return "server_input";
        case MechanismSlot::change_feed: return "change_feed";
        case MechanismSlot::tasks: return "tasks";
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

// ---- server_input

// `input_required_result`: the call's result asks (a result of the type InputRequiredResult, with the requests and a
// state), and the client retries the call with the answers and the state beside the call's own params. A request may
// ask exactly where its params type has the answers member: the revision's schema puts it on the requests that may.
class InputRequiredResult final : public InputMechanism {
public:
    static constexpr std::string_view result_type = "InputRequiredResult";
    static constexpr std::string_view responses_member = "inputResponses";
    static constexpr std::string_view state_member = "requestState";
    static constexpr std::string_view requests_member = "inputRequests";
    static constexpr std::string_view result_type_member = "resultType";
    static constexpr std::string_view input_required = "input_required";

    std::optional<std::string> check(const profile::Profile& profile) const override {
        const profile::Type* type = profile.type(result_type);
        if (type == nullptr) return "the profile has no type " + std::string(result_type);
        for (const auto name : {requests_member, state_member, result_type_member}) {
            if (!type->has_property(name)) {
                return "the profile's " + std::string(result_type) + " has no member " + std::string(name);
            }
        }
        if (profile.input_requests().empty()) return "the profile lists no input requests";
        return std::nullopt;
    }

    InputDelivery delivery() const override { return InputDelivery::result; }

    bool asks_on(const profile::Profile& profile, std::string_view method) const override {
        const profile::Message* request = profile.request(method);
        if (request == nullptr || !request->from_client || !request->params) return false;
        const profile::Type* params = profile.type(*request->params);
        return params != nullptr && params->has_property(responses_member) && params->has_property(state_member);
    }

    bool carries(const profile::Profile& profile, std::string_view method) const override {
        return profile.input_request(method) != nullptr;
    }

    std::optional<std::string> params_type(const profile::Profile& profile, std::string_view method) const override {
        const profile::InputRequest* request = profile.input_request(method);
        return request == nullptr ? std::nullopt : request->params;
    }

    std::span<const std::string> params_required(const profile::Profile& profile,
                                                 std::string_view method) const override {
        const profile::InputRequest* request = profile.input_request(method);
        if (request == nullptr) return {};
        return request->params_required;
    }

    std::variant<InputRetry, RequestError> retry(const Message& request, const profile::Profile& profile) const override {
        InputRetry retry;
        if (!asks_on(profile, request.method)) return retry;
        if (const json* responses = member(request.params, responses_member)) {
            if (!responses->is_object()) {
                return invalid_params("Invalid params: params." + std::string(responses_member) + " must be an object");
            }
            retry.responses = *responses;
        }
        if (const json* state = member(request.params, state_member)) retry.state = *state;
        return retry;
    }

    json asking_result(json requests, std::string state) const override {
        json body = json::object();
        body[std::string(result_type_member)] = std::string(input_required);
        if (requests.is_object() && !requests.empty()) body[std::string(requests_member)] = std::move(requests);
        body[std::string(state_member)] = std::move(state);
        return body;
    }

    std::string_view asking_result_type() const override { return result_type; }

    json asking_request(const RequestId&, std::string, json) const override { return json(); }
};

// `server_request`: the session sends each request to the client as a JSON-RPC request of its own, on the stream of
// the call that asks, and waits for the client's responses.
class ServerRequest final : public InputMechanism {
public:
    std::optional<std::string> check(const profile::Profile&) const override { return std::nullopt; }
    InputDelivery delivery() const override { return InputDelivery::requests; }

    // The revisions that send requests put no limit on the requests a server may send them during.
    bool asks_on(const profile::Profile&, std::string_view) const override { return true; }

    bool carries(const profile::Profile& profile, std::string_view method) const override {
        const profile::Message* request = profile.request(method);
        return request != nullptr && request->from_server;
    }

    std::optional<std::string> params_type(const profile::Profile& profile, std::string_view method) const override {
        const profile::Message* request = profile.request(method);
        return request == nullptr ? std::nullopt : request->params;
    }

    std::span<const std::string> params_required(const profile::Profile& profile,
                                                 std::string_view method) const override {
        const profile::Message* request = profile.request(method);
        if (request == nullptr) return {};
        return request->params_required;
    }

    std::variant<InputRetry, RequestError> retry(const Message&, const profile::Profile&) const override {
        return InputRetry{};
    }

    json asking_result(json, std::string) const override { return json(); }
    std::string_view asking_result_type() const override { return {}; }

    json asking_request(const RequestId& id, std::string method, json params) const override {
        json message = json::object();
        message["jsonrpc"] = "2.0";
        message["id"] = id.to_json();
        message["method"] = std::move(method);
        if (!params.is_null()) message["params"] = std::move(params);
        return message;
    }
};

// ---- change_feed

// What both mechanisms write about a change: the notifications every revision names the same way.
class FeedNotifications {
public:
    static constexpr std::string_view tools_changed = "notifications/tools/list_changed";
    static constexpr std::string_view prompts_changed = "notifications/prompts/list_changed";
    static constexpr std::string_view resources_changed = "notifications/resources/list_changed";
    static constexpr std::string_view resource_updated = "notifications/resources/updated";

    static FeedMessage of(const Change& change) {
        FeedMessage message;
        switch (change.kind) {
            case ChangeKind::tools_list: message.method = tools_changed; break;
            case ChangeKind::prompts_list: message.method = prompts_changed; break;
            case ChangeKind::resources_list: message.method = resources_changed; break;
            case ChangeKind::resource_updated:
                message.method = resource_updated;
                message.params["uri"] = change.uri;
                break;
        }
        return message;
    }

    static std::variant<FeedRequest, RequestError> uri_request(const Message& request, FeedRequest::Action action) {
        const json* uri = member(request.params, "uri");
        const auto* text = uri == nullptr ? nullptr : uri->get_ptr<const std::string*>();
        if (text == nullptr) return invalid_params("Invalid params: params.uri must be a string");
        FeedRequest asked;
        asked.action = action;
        asked.uri = *text;
        return asked;
    }
};

// `connection_notifications`: the notifications of a change go to the connection a handshake opened, once the client
// said with `notifications/initialized` that it is ready; resources/subscribe and resources/unsubscribe add a URI to
// the resources the connection watches and remove it.
class ConnectionNotifications final : public ChangeFeedMechanism {
public:
    static constexpr std::string_view subscribe_method = "resources/subscribe";
    static constexpr std::string_view unsubscribe_method = "resources/unsubscribe";
    static constexpr std::string_view ready_method = "notifications/initialized";

    std::optional<std::string> check(const profile::Profile& profile) const override {
        const profile::Message* ready = profile.notification(ready_method);
        if (ready == nullptr || !ready->from_client) {
            return "the profile has no client notification " + std::string(ready_method);
        }
        return std::nullopt;
    }

    FeedDelivery delivery() const override { return FeedDelivery::connection; }

    bool serves(std::string_view method) const override {
        return method == subscribe_method || method == unsubscribe_method;
    }

    std::variant<FeedRequest, RequestError> read(const Message& request) const override {
        return FeedNotifications::uri_request(
            request, request.method == subscribe_method ? FeedRequest::Action::watch : FeedRequest::Action::unwatch);
    }

    bool readies(const Message& notification) const override { return notification.method == ready_method; }

    FeedMessage notification(const Change& change) const override { return FeedNotifications::of(change); }

    FeedMessage acknowledgement(const ChangeFilter&) const override { return {}; }
    void tag(json&, const RequestId&) const override {}
    std::optional<FeedMessage> ending_notice(const RequestId&) const override { return std::nullopt; }
    json ending_result() const override { return json::object(); }
    std::string_view ending_result_type() const override { return {}; }
};

// `subscriptions_listen`: subscriptions/listen opens a subscription whose answer is a stream. Its first message is the
// acknowledgement, which names what of the request's filter the server honours; every message of it carries the
// subscription's id (the listen request's id) in `_meta`; the server ends it with notifications/cancelled naming the
// listen request and then a result that marks the end as graceful.
class SubscriptionsListen final : public ChangeFeedMechanism {
public:
    static constexpr std::string_view listen_method = "subscriptions/listen";
    static constexpr std::string_view acknowledged_method = "notifications/subscriptions/acknowledged";
    static constexpr std::string_view cancelled_method = "notifications/cancelled";
    static constexpr std::string_view subscription_key = "io.modelcontextprotocol/subscriptionId";
    static constexpr std::string_view notification_meta_type = "NotificationMetaObject";
    static constexpr std::string_view filter_member = "notifications";
    static constexpr std::string_view filter_type = "SubscriptionFilter";
    static constexpr std::string_view result_type = "SubscriptionsListenResult";
    static constexpr std::string_view tools_member = "toolsListChanged";
    static constexpr std::string_view prompts_member = "promptsListChanged";
    static constexpr std::string_view resources_member = "resourcesListChanged";
    static constexpr std::string_view updates_member = "resourceSubscriptions";
    static constexpr std::string_view ending_reason = "The server ended the subscription";

    std::optional<std::string> check(const profile::Profile& profile) const override {
        const profile::Message* listen = profile.request(listen_method);
        if (listen == nullptr || !listen->from_client) {
            return "the profile has no client request " + std::string(listen_method);
        }
        const profile::Message* acknowledged = profile.notification(acknowledged_method);
        if (acknowledged == nullptr || !acknowledged->from_server) {
            return "the profile has no server notification " + std::string(acknowledged_method);
        }
        const profile::Type* meta = profile.type(notification_meta_type);
        if (meta == nullptr || !meta->has_property(subscription_key)) {
            return "the profile's " + std::string(notification_meta_type) + " has no member " +
                   std::string(subscription_key);
        }
        for (const auto name : {filter_type, result_type}) {
            if (profile.type(name) == nullptr) return "the profile has no type " + std::string(name);
        }
        return std::nullopt;
    }

    FeedDelivery delivery() const override { return FeedDelivery::subscription; }

    bool serves(std::string_view method) const override { return method == listen_method; }

    std::variant<FeedRequest, RequestError> read(const Message& request) const override {
        const json* filter = member(request.params, filter_member);
        if (filter == nullptr || !filter->is_object()) {
            return invalid_params("Invalid params: params." + std::string(filter_member) + " must be an object");
        }
        FeedRequest asked;
        asked.action = FeedRequest::Action::listen;
        const auto flag = [&](std::string_view name, bool& out) -> std::optional<RequestError> {
            const json* value = member(*filter, name);
            if (value == nullptr) return std::nullopt;
            if (!value->is_boolean()) {
                return invalid_params("Invalid params: params." + std::string(filter_member) + "." + std::string(name) +
                                      " must be a boolean");
            }
            out = value->get<bool>();
            return std::nullopt;
        };
        if (auto error = flag(tools_member, asked.filter.tools_list)) return *error;
        if (auto error = flag(prompts_member, asked.filter.prompts_list)) return *error;
        if (auto error = flag(resources_member, asked.filter.resources_list)) return *error;
        if (const json* uris = member(*filter, updates_member)) {
            const auto* list = uris->get_ptr<const json::array_t*>();
            if (list == nullptr) {
                return invalid_params("Invalid params: params." + std::string(filter_member) + "." +
                                      std::string(updates_member) + " must be an array of strings");
            }
            for (const json& uri : *list) {
                const auto* text = uri.get_ptr<const std::string*>();
                if (text == nullptr) {
                    return invalid_params("Invalid params: params." + std::string(filter_member) + "." +
                                          std::string(updates_member) + " must be an array of strings");
                }
                auto& kept = asked.filter.resources;
                if (std::find(kept.begin(), kept.end(), *text) == kept.end()) kept.push_back(*text);
            }
        }
        return asked;
    }

    bool readies(const Message&) const override { return false; }

    FeedMessage notification(const Change& change) const override { return FeedNotifications::of(change); }

    FeedMessage acknowledgement(const ChangeFilter& honoured) const override {
        json filter = json::object();
        if (honoured.tools_list) filter[std::string(tools_member)] = true;
        if (honoured.prompts_list) filter[std::string(prompts_member)] = true;
        if (honoured.resources_list) filter[std::string(resources_member)] = true;
        if (!honoured.resources.empty()) {
            json uris = json::array();
            for (const auto& uri : honoured.resources) uris.push_back(uri);
            filter[std::string(updates_member)] = std::move(uris);
        }
        FeedMessage message;
        message.method = acknowledged_method;
        message.params[std::string(filter_member)] = std::move(filter);
        return message;
    }

    void tag(json& members, const RequestId& id) const override {
        json& meta = members["_meta"];
        if (!meta.is_object()) meta = json::object();
        meta[std::string(subscription_key)] = id.to_json();
    }

    std::optional<FeedMessage> ending_notice(const RequestId& id) const override {
        FeedMessage message;
        message.method = cancelled_method;
        message.params["requestId"] = id.to_json();
        message.params["reason"] = std::string(ending_reason);
        return message;
    }

    json ending_result() const override { return json::object(); }
    std::string_view ending_result_type() const override { return result_type; }
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
    registry.add_input("input_required_result", factory<InputMechanism, InputRequiredResult>());
    registry.add_input("server_request", factory<InputMechanism, ServerRequest>());
    registry.add_feed("connection_notifications", factory<ChangeFeedMechanism, ConnectionNotifications>());
    registry.add_feed("subscriptions_listen", factory<ChangeFeedMechanism, SubscriptionsListen>());
    registry.add_tasks("none", [] { return detail::make_no_tasks(); });
    registry.add_tasks("augmented_request", [] { return detail::make_augmented_request_tasks(); });
    registry.add_tasks("server_directed", [] { return detail::make_server_directed_tasks(); });
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
void MechanismRegistry::add_input(std::string name, Factory<InputMechanism> factory) {
    put(MechanismSlot::server_input, std::move(name), std::move(factory));
}
void MechanismRegistry::add_feed(std::string name, Factory<ChangeFeedMechanism> factory) {
    put(MechanismSlot::change_feed, std::move(name), std::move(factory));
}
void MechanismRegistry::add_tasks(std::string name, Factory<TaskMechanism> factory) {
    put(MechanismSlot::tasks, std::move(name), std::move(factory));
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
std::unique_ptr<InputMechanism> MechanismRegistry::make_input(std::string_view name) const {
    return make<InputMechanism>(MechanismSlot::server_input, name);
}
std::unique_ptr<ChangeFeedMechanism> MechanismRegistry::make_feed(std::string_view name) const {
    return make<ChangeFeedMechanism>(MechanismSlot::change_feed, name);
}
std::unique_ptr<TaskMechanism> MechanismRegistry::make_tasks(std::string_view name) const {
    return make<TaskMechanism>(MechanismSlot::tasks, name);
}

const MechanismRegistry& builtin_mechanisms() {
    static const MechanismRegistry registry = make_builtin();
    return registry;
}

}  // namespace neomcp
