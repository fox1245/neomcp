#include "neomcp/jsonrpc.h"

#include <utility>

#include "core/large_json_cache.h"

namespace neomcp {

std::optional<RequestId> RequestId::from_json(const json& value) {
    // neojson reports an unsigned value as an integer too, so the unsigned case is read first.
    if (value.is_number_unsigned()) return RequestId(*value.get_ptr<const std::uint64_t*>());
    if (value.is_number_integer()) return RequestId(*value.get_ptr<const std::int64_t*>());
    if (const auto* text = value.get_ptr<const std::string*>()) return RequestId(*text);
    return std::nullopt;
}

json RequestId::to_json() const {
    if (const auto* text = std::get_if<std::string>(&value_)) return json(*text);
    if (const auto* number = std::get_if<std::uint64_t>(&value_)) return json(*number);
    return json(std::get<std::int64_t>(value_));
}

namespace {

Message invalid(std::string problem, std::optional<RequestId> id) {
    Message message;
    message.kind = MessageKind::invalid;
    message.problem = std::move(problem);
    message.id = std::move(id);
    return message;
}

}  // namespace

Message classify(json value) {
    auto* object = value.get_ptr<json::object_t*>();
    if (object == nullptr) return invalid("a JSON-RPC message is an object", std::nullopt);

    const auto id_member = object->find("id");
    const bool has_id = id_member != object->end();
    std::optional<RequestId> id;
    if (has_id) id = RequestId::from_json(id_member->second);

    const auto version = object->find("jsonrpc");
    const auto* version_text = version == object->end() ? nullptr : version->second.get_ptr<const std::string*>();
    if (version_text == nullptr || *version_text != "2.0") return invalid("\"jsonrpc\" must be \"2.0\"", id);

    const auto method = object->find("method");
    if (method != object->end()) {
        const auto* name = method->second.get_ptr<const std::string*>();
        if (name == nullptr) return invalid("\"method\" must be a string", id);
        Message message;
        message.method = *name;
        const auto params = object->find("params");
        if (params != object->end()) {
            if (!params->second.is_object() && !params->second.is_array()) {
                return invalid("\"params\" must be an object or an array", id);
            }
            message.params = std::move(params->second);
        }
        if (has_id) {
            if (!id) return invalid("\"id\" must be a string or an integer", std::nullopt);
            message.kind = MessageKind::request;
            message.id = std::move(id);
        } else {
            message.kind = MessageKind::notification;
        }
        return message;
    }

    const auto result = object->find("result");
    const auto error = object->find("error");
    const bool has_result = result != object->end();
    const bool has_error = error != object->end();
    if (has_id && has_result != has_error) {
        Message message;
        message.kind = MessageKind::response;
        message.id = std::move(id);
        message.failed = has_error;
        message.outcome = std::move(has_error ? error->second : result->second);
        return message;
    }
    return invalid("the message is neither a request, a notification nor a response", id);
}

bool nesting_exceeds(std::string_view text, std::size_t limit) noexcept {
    std::size_t depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (const char c : text) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        switch (c) {
            case '"': in_string = true; break;
            case '[':
            case '{':
                if (++depth > limit) return true;
                break;
            case ']':
            case '}':
                if (depth > 0) --depth;
                break;
            default: break;
        }
    }
    return false;
}

json make_result(const RequestId& id, json result) {
    json message = json::object();
    message["jsonrpc"] = "2.0";
    message["id"] = id.to_json();
    message["result"] = std::move(result);
    return message;
}

json make_error(const std::optional<RequestId>& id, int code, std::string text, json data) {
    json error = json::object();
    error["code"] = code;
    error["message"] = std::move(text);
    if (!data.is_null()) error["data"] = std::move(data);
    json message = json::object();
    message["jsonrpc"] = "2.0";
    message["id"] = id ? id->to_json() : json(nullptr);
    message["error"] = std::move(error);
    return message;
}

json make_notification(std::string method, json params) {
    json message = json::object();
    message["jsonrpc"] = "2.0";
    message["method"] = std::move(method);
    if (!params.is_null()) message["params"] = std::move(params);
    return message;
}

std::string serialize(const json& message) {
    detail::LargeJsonCacheGuard trim_caches(true);
    std::string text = message.dump(-1, ' ', false, json::error_handler_t::replace);
    trim_caches.active = text.size() >= detail::large_json_trim_threshold;
    return text;
}

}  // namespace neomcp
