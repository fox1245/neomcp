// What JSON Schema says about values, independent of any schema: numbers compare by value whatever their JSON
// representation, equality ignores the order of object members, and "integer" includes 1.0.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "neomcp/json.h"

namespace neomcp::json_schema::detail {

enum class Order { less, equal, greater, unordered };

// Whether `value`, which must be a number, has no fractional part. A float counts when it is finite.
bool is_integral(const json& value);

// Compare two numbers exactly: 64-bit integers of either sign and doubles never lose precision against each other.
// `unordered` is returned when a double is not a number.
Order compare_numbers(const json& a, const json& b);

// Whether `value` is an integral multiple of `divisor`, decided on the decimal numbers the two values print as. Both
// must be numbers and `divisor` must be finite and greater than zero; a `value` that is not finite is no multiple.
bool is_multiple_of(const json& value, const json& divisor);

// The count a non-negative integral number stands for, saturated at the largest std::size_t; nothing for any other
// value.
std::optional<std::size_t> to_count(const json& value);

// The result of a comparison that may run into a nesting limit.
enum class Equality { equal, different, too_deep };

// JSON Schema equality: numbers by value, objects by their members whatever the order. Arrays and objects nested
// more than `max_depth` levels end the comparison with `too_deep`.
Equality equal(const json& a, const json& b, std::size_t max_depth);

// A hash that agrees with `equal`: values that are equal have the same hash. Nothing when a value nests more than
// `max_depth` levels of arrays and objects.
std::optional<std::uint64_t> hash_value(const json& value, std::size_t max_depth);

// The JSON type name of a value, for messages.
std::string_view type_name(const json& value);

}  // namespace neomcp::json_schema::detail
