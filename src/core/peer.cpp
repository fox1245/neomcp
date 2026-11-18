#include "neomcp/testing/peer.h"

#include <stdexcept>
#include <utility>
#include <variant>

namespace neomcp::testing {
namespace {

// Protocol identifiers the helpers fill in; whether a revision has them is read from its profile.
constexpr std::string_view meta_type = "RequestMetaObject";
constexpr std::string_view version_key = "io.modelcontextprotocol/protocolVersion";
constexpr std::string_view capabilities_key = "io.modelcontextprotocol/clientCapabilities";

std::string id_key(const json& id) { return serialize(id); }

}  // namespace

Peer::Peer(ToolHost& host, SessionOptions options)
    : Peer(Connector{[&host, &options](Sink sink) {
          return Session::create(std::move(options), host, std::move(sink));
      }}) {}

Peer::Peer(Connector connector) {
    auto made = connector.open([this](std::string line) { deliver(std::move(line)); });
    if (const auto* error = std::get_if<CreateError>(&made)) throw std::runtime_error(error->message);
    session_ = std::move(std::get<std::unique_ptr<Session>>(made));
}

Peer::~Peer() { session_.reset(); }

void Peer::deliver(std::string line) {
    json value = json::parse(line, nullptr, false);
    const std::lock_guard<std::mutex> lock(mutex_);
    transcript_.push_back(value);
    const auto* object = value.get_ptr<const json::object_t*>();
    if (object != nullptr && object->find("method") != object->end()) {
        notifications_.push_back(std::move(value));
    } else if (object != nullptr && object->find("id") != object->end() && !object->find("id")->second.is_null()) {
        responses_[id_key(object->find("id")->second)].push_back(std::move(value));
    } else {
        others_.push_back(std::move(value));
    }
    arrived_.notify_all();
}

std::optional<json> Peer::request(const json& message, Timeout timeout) {
    const auto* object = message.get_ptr<const json::object_t*>();
    if (object == nullptr || object->find("id") == object->end()) {
        throw std::logic_error("Peer::request needs a message with an id");
    }
    const auto id = RequestId::from_json(object->find("id")->second);
    if (!id) throw std::logic_error("Peer::request needs a string or integer id");
    send(serialize(message));
    return await_response(*id, timeout);
}

std::optional<json> Peer::await_response(const RequestId& id, Timeout timeout) {
    const std::string key = id_key(id.to_json());
    std::unique_lock<std::mutex> lock(mutex_);
    const bool arrived = arrived_.wait_for(lock, timeout, [&] {
        const auto found = responses_.find(key);
        return found != responses_.end() && !found->second.empty();
    });
    if (!arrived) return std::nullopt;
    auto& queue = responses_[key];
    json response = std::move(queue.front());
    queue.pop_front();
    return response;
}

void Peer::notify(const json& message) { send(serialize(message)); }

void Peer::send(std::string_view line) {
    const std::lock_guard<std::mutex> lock(send_mutex_);
    session_->receive(line);
}

std::optional<json> Peer::next_notification(Timeout timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!arrived_.wait_for(lock, timeout, [&] { return !notifications_.empty(); })) return std::nullopt;
    json value = std::move(notifications_.front());
    notifications_.pop_front();
    return value;
}

std::optional<json> Peer::next_other(Timeout timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!arrived_.wait_for(lock, timeout, [&] { return !others_.empty(); })) return std::nullopt;
    json value = std::move(others_.front());
    others_.pop_front();
    return value;
}

std::vector<json> Peer::transcript() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return transcript_;
}

void Peer::close() { session_->close(); }

json Peer::initialize_request(const profile::Profile& profile, const RequestId& id, json capabilities) {
    const profile::Message* entry = profile.request("initialize");
    if (entry == nullptr || !entry->from_client) {
        throw std::logic_error("revision " + profile.revision() + " has no initialize request");
    }
    json client = json::object();
    client["name"] = "neomcp-test-peer";
    client["version"] = "0.0.0";
    json params = json::object();
    params["protocolVersion"] = profile.revision();
    params["capabilities"] = std::move(capabilities);
    params["clientInfo"] = std::move(client);
    json message = json::object();
    message["jsonrpc"] = "2.0";
    message["id"] = id.to_json();
    message["method"] = "initialize";
    message["params"] = std::move(params);
    return message;
}

std::optional<json> Peer::initialize(const profile::Profile& profile, Timeout timeout) {
    auto response = request(initialize_request(profile, RequestId("initialize")), timeout);
    if (response) {
        json initialized = json::object();
        initialized["jsonrpc"] = "2.0";
        initialized["method"] = "notifications/initialized";
        notify(initialized);
    }
    return response;
}

json Peer::request_meta(const profile::Profile& profile, json capabilities) {
    const profile::Type* type = profile.type(meta_type);
    if (type == nullptr) throw std::logic_error("revision " + profile.revision() + " has no " + std::string(meta_type));
    json meta = json::object();
    for (const auto& key : type->required) {
        if (key == version_key) {
            meta[key] = profile.revision();
        } else if (key == capabilities_key) {
            meta[key] = capabilities;
        } else {
            throw std::logic_error("Peer::request_meta cannot fill the required _meta key " + key);
        }
    }
    return meta;
}

json Peer::make_request(const profile::Profile& profile, const RequestId& id, std::string method, json params) {
    if (params.is_null()) params = json::object();
    if (profile.type(meta_type) != nullptr && params.is_object()) {
        json& meta = params["_meta"];
        if (meta.is_null()) meta = json::object();
        const json named = request_meta(profile);
        for (const auto& entry : *named.get_ptr<const json::object_t*>()) meta[entry.first] = entry.second;
    }
    json message = json::object();
    message["jsonrpc"] = "2.0";
    message["id"] = id.to_json();
    message["method"] = std::move(method);
    message["params"] = std::move(params);
    return message;
}

}  // namespace neomcp::testing
