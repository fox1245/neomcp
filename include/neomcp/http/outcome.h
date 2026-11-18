// The result of an operation that can fail with a message for a person to read.
#pragma once

#include <optional>
#include <string>
#include <utility>

namespace neomcp::http {

// Holds a value, or the reason there is none. No operation that returns an Outcome throws for the failure it reports.
template <class T>
struct Outcome {
    std::optional<T> value;
    std::string error;  // non-empty exactly when `value` is empty

    explicit operator bool() const noexcept { return value.has_value(); }

    static Outcome success(T result) { return Outcome{std::optional<T>(std::move(result)), {}}; }
    static Outcome failure(std::string reason) { return Outcome{std::nullopt, std::move(reason)}; }
};

}  // namespace neomcp::http
