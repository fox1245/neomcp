// JSON-RPC 2.0 messages: request ids, the classification of a parsed message, and builders for what a server
// writes. Nothing here throws on bad input; a message that is not valid JSON-RPC is classified as invalid.
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "neomcp/json.h"

namespace neomcp {

// The error codes JSON-RPC 2.0 defines. Every other code comes from a profile's `errors`.
namespace error_code {
inline constexpr int parse_error = -32700;
inline constexpr int invalid_request = -32600;
inline constexpr int method_not_found = -32601;
inline constexpr int invalid_params = -32602;
inline constexpr int internal_error = -32603;
}  // namespace error_code

// A request id: a string or an integer. Two ids are equal when they are the same string or the same integer,
// whatever C++ type the integer was given in. Integers cover the range of int64 and uint64.
class RequestId {
public:
    template <std::integral Integer>
        requires(!std::same_as<Integer, bool>)
    RequestId(Integer value) {
        if constexpr (std::is_signed_v<Integer>) {
            if (value < 0) {
                value_ = static_cast<std::int64_t>(value);
                return;
            }
        }
        value_ = static_cast<std::uint64_t>(value);
    }
    RequestId(std::string value) : value_(std::move(value)) {}
    RequestId(const char* value) : value_(std::string(value)) {}

    // The id a JSON value holds; empty unless the value is a string or an integer.
    static std::optional<RequestId> from_json(const json& value);

    json to_json() const;
    bool is_string() const noexcept { return std::holds_alternative<std::string>(value_); }

    friend bool operator==(const RequestId& a, const RequestId& b) { return a.value_ == b.value_; }
    friend bool operator<(const RequestId& a, const RequestId& b) { return a.value_ < b.value_; }

private:
    // Negative integers are kept as int64 and all others as uint64, so equal numbers compare equal.
    std::variant<std::int64_t, std::uint64_t, std::string> value_;
};

enum class MessageKind { request, notification, response, invalid };

// A parsed message, classified. For an invalid message `problem` says why and `id` holds the message's id when it
// could be read, so that the error can be answered to it.
struct Message {
    MessageKind kind = MessageKind::invalid;
    std::optional<RequestId> id;  // requests always; responses unless the id is null; invalid when readable
    std::string method;           // requests and notifications
    json params;                  // null when the message has none; else an object or an array
    std::string problem;          // invalid messages only
    // Responses only: the `result` member, or the `error` member when `failed` is set.
    json outcome;
    bool failed = false;
};

// Classifies a parsed JSON value. Takes the value so that params can be moved out of it.
Message classify(json value);

// A text whose JSON nesting (arrays and objects inside each other) goes deeper than `limit`. It reads only the
// brackets outside strings, so that a deeply nested text is refused before it is parsed.
bool nesting_exceeds(std::string_view text, std::size_t limit) noexcept;

// What a server writes.
json make_result(const RequestId& id, json result);
// `id` empty writes "id": null, for errors about a message whose id could not be read.
json make_error(const std::optional<RequestId>& id, int code, std::string message, json data = nullptr);
json make_notification(std::string method, json params);

// One message on one line: compact JSON, never a raw newline. Text that is not valid UTF-8 is replaced, so this
// does not throw for bad strings.
std::string serialize(const json& message);

}  // namespace neomcp
