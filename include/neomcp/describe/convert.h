// Conversion between C++ values and JSON values, for every type that has a schema (see schema.h).
//
//   T value = neomcp::from_json<T>(document);   // throws DecodeError
//   neomcp::json document = neomcp::to_json(value);
//
// Decoding:
//  - A wrong JSON type, a missing required member, an integer outside the C++ type's range, a number with a
//    fractional part for an integer type, an enumerator name that is not declared and a non-object where a struct
//    is expected throw DecodeError, which says where (a JSON Pointer) and what was expected there.
//  - A number with no fractional part (3.0) is accepted for an integer type if it is in range. A floating point
//    type accepts any JSON number, integral ones included, and a float rejects a number it cannot hold.
//  - An absent member of type std::optional decodes to std::nullopt, and so does a JSON null.
//  - Members the struct does not describe are ignored. Rejecting them is the job of schema validation.
//  - Field options (minimum, one_of, ...) are NOT enforced: they are part of the schema, and validating a document
//    against the schema is what enforces them.
//
// Encoding writes members in declaration order, leaves out a std::optional member that holds nothing, and writes an
// enum by its declared name (std::invalid_argument for a value that has none).
//
// A type of your own with its own Schema specialisation converts through neojson's customisation points:
// adl_serializer<T> or the functions to_json(json&, const T&) and from_json(const json&, T&) found by argument
// dependent lookup. An exception of neojson thrown by such a conversion is reported as a DecodeError at the place
// being decoded. A described struct or enum is converted by its description even if it also has a Schema.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "neomcp/describe/model.h"
#include "neomcp/describe/schema.h"
#include "neomcp/json.h"

namespace neomcp {

// A document that does not have the shape the type expects.
class DecodeError : public std::runtime_error {
public:
    DecodeError(std::string pointer, std::string expected)
        : std::runtime_error("decoding failed at \"" + pointer + "\": expected " + expected),
          pointer_(std::move(pointer)),
          expected_(std::move(expected)) {}

    // RFC 6901 JSON Pointer to the offending place ("" is the whole document). '~' and '/' in member names are
    // written "~0" and "~1". For a missing member the pointer names the member that should be there, so it points
    // at a place that does not exist in the document.
    const std::string& pointer() const noexcept { return pointer_; }
    // What was expected at that place.
    const std::string& expected() const noexcept { return expected_; }

private:
    std::string pointer_;
    std::string expected_;
};

namespace detail {

// The place being decoded, as a chain of steps from the root that lives on the stack while decoding goes down.
struct PathNode {
    const PathNode* parent;
    std::string_view key;  // a member name, unless is_index
    std::size_t index;
    bool is_index;
};

inline std::string render_pointer(const PathNode* leaf) {
    std::vector<const PathNode*> chain;
    for (const PathNode* step = leaf; step != nullptr; step = step->parent) {
        chain.push_back(step);
    }
    std::string pointer;
    for (auto step = chain.rbegin(); step != chain.rend(); ++step) {
        pointer += '/';
        if ((*step)->is_index) {
            pointer += std::to_string((*step)->index);
            continue;
        }
        for (const char c : (*step)->key) {
            if (c == '~') {
                pointer += "~0";
            } else if (c == '/') {
                pointer += "~1";
            } else {
                pointer += c;
            }
        }
    }
    return pointer;
}

[[noreturn]] inline void fail(const PathNode* path, std::string expected) {
    throw DecodeError(render_pointer(path), std::move(expected));
}

// What a value of the type is called in a message.
template <class T>
constexpr std::string_view kind_name() {
    switch (kind_of<T>()) {
        case Kind::any_json:
            return "any JSON value";
        case Kind::boolean:
            return "boolean";
        case Kind::integer:
            return "integer";
        case Kind::floating:
            return "number";
        case Kind::string:
        case Kind::enumeration:
            return "string";
        case Kind::array:
            return "array";
        case Kind::dictionary:
        case Kind::object:
            return "object";
        case Kind::optional:
        case Kind::unsupported:
            break;
    }
    return "value accepted by its conversion";
}

template <class T>
bool fits_unsigned(std::uint64_t value) {
    return value <= static_cast<std::uint64_t>(std::numeric_limits<T>::max());
}

template <class T>
bool fits_signed(std::int64_t value) {
    if constexpr (std::is_signed_v<T>) {
        return value >= static_cast<std::int64_t>(std::numeric_limits<T>::min()) &&
               value <= static_cast<std::int64_t>(std::numeric_limits<T>::max());
    } else {
        return value >= 0 && static_cast<std::uint64_t>(value) <= static_cast<std::uint64_t>(std::numeric_limits<T>::max());
    }
}

template <class T>
std::string integer_range() {
    if constexpr (std::is_signed_v<T>) {
        return "integer from " + std::to_string(static_cast<long long>(std::numeric_limits<T>::min())) + " to " +
               std::to_string(static_cast<long long>(std::numeric_limits<T>::max()));
    } else {
        return "integer from 0 to " + std::to_string(static_cast<unsigned long long>(std::numeric_limits<T>::max()));
    }
}

template <class T>
T decode_integer(const json& value, const PathNode* path) {
    switch (value.type()) {
        case json::value_t::number_unsigned: {
            const std::uint64_t number = *value.get_ptr<const std::uint64_t*>();
            if (!fits_unsigned<T>(number)) {
                fail(path, integer_range<T>());
            }
            return static_cast<T>(number);
        }
        case json::value_t::number_integer: {
            const std::int64_t number = *value.get_ptr<const std::int64_t*>();
            if (!fits_signed<T>(number)) {
                fail(path, integer_range<T>());
            }
            return static_cast<T>(number);
        }
        case json::value_t::number_float: {
            const double number = *value.get_ptr<const double*>();
            if (!std::isfinite(number) || std::floor(number) != number) {
                fail(path, "integer");
            }
            // The range of T is [-2^digits, 2^digits) for signed types and [0, 2^digits) for unsigned ones; both
            // bounds are exact in a double.
            const double limit = std::ldexp(1.0, std::numeric_limits<T>::digits);
            double lowest = 0.0;
            if constexpr (std::is_signed_v<T>) {
                lowest = -limit;
            }
            if (number < lowest || number >= limit) {
                fail(path, integer_range<T>());
            }
            return static_cast<T>(number);
        }
        default:
            fail(path, "integer");
    }
}

template <class T>
std::string enumerator_list() {
    std::string list = "one of ";
    bool first = true;
    for (const auto& entry : DescribedEnum<T>::values) {
        if (!first) {
            list += ", ";
        }
        first = false;
        list += '"';
        list += entry.name;
        list += '"';
    }
    return list;
}

template <class T>
T decode(const json& value, const PathNode* path);

template <class T, class F>
void decode_member(T& result, const json& object, const PathNode* path, const F& item) {
    static_assert(std::is_base_of_v<typename F::class_type, T>,
                  "neomcp: a member listed for a described struct belongs to an unrelated class");
    using Member = typename F::member_type;
    using Core = optional_core_t<Member>;
    static_assert(has_schema<Core>(), "neomcp: no neomcp::Schema for this member type");
    const PathNode node{path, item.name, 0, false};
    const auto found = object.find(item.name);
    if (found == object.end()) {
        if constexpr (is_optional_v<Member>) {
            result.*(item.pointer) = std::nullopt;
            return;
        } else {
            fail(&node, "required member of type " + std::string(kind_name<Member>()));
        }
    }
    if constexpr (is_optional_v<Member>) {
        if (found->is_null()) {
            result.*(item.pointer) = std::nullopt;
            return;
        }
    }
    result.*(item.pointer) = decode<Core>(*found, &node);
}

template <class T>
T decode(const json& value, const PathNode* path) {
    constexpr Kind kind = kind_of<T>();
    if constexpr (kind == Kind::any_json) {
        return value;
    } else if constexpr (kind == Kind::boolean) {
        if (!value.is_boolean()) {
            fail(path, "boolean");
        }
        return value.get<bool>();
    } else if constexpr (kind == Kind::integer) {
        return decode_integer<T>(value, path);
    } else if constexpr (kind == Kind::floating) {
        if (!value.is_number()) {
            fail(path, "number");
        }
        const double number = value.get<double>();
        if constexpr (std::is_same_v<T, float>) {
            if (std::fabs(number) > static_cast<double>(std::numeric_limits<float>::max())) {
                fail(path, "number that a float can hold");
            }
        }
        return static_cast<T>(number);
    } else if constexpr (kind == Kind::string) {
        if (!value.is_string()) {
            fail(path, "string");
        }
        return *value.get_ptr<const std::string*>();
    } else if constexpr (kind == Kind::array) {
        if (!value.is_array()) {
            fail(path, "array");
        }
        using Element = typename vector_traits<T>::element;
        T result;
        result.reserve(value.size());
        std::size_t index = 0;
        for (const json& item : value) {
            const PathNode node{path, {}, index++, true};
            result.push_back(decode<Element>(item, &node));
        }
        return result;
    } else if constexpr (kind == Kind::dictionary) {
        if (!value.is_object()) {
            fail(path, "object");
        }
        using Element = typename string_map_traits<T>::element;
        T result;
        for (const auto& item : value.items()) {
            const std::string& key = item.key();
            const PathNode node{path, key, 0, false};
            result.emplace(key, decode<Element>(item.value(), &node));
        }
        return result;
    } else if constexpr (kind == Kind::enumeration) {
        if (!value.is_string()) {
            fail(path, "string");
        }
        const std::string& name = *value.get_ptr<const std::string*>();
        for (const auto& entry : DescribedEnum<T>::values) {
            if (entry.name == name) {
                return entry.value;
            }
        }
        fail(path, enumerator_list<T>());
    } else if constexpr (kind == Kind::object) {
        static_assert(std::is_default_constructible_v<T>, "neomcp: a described struct must be default constructible to be decoded");
        if (!value.is_object()) {
            fail(path, "object");
        }
        T result{};
        std::apply([&](const auto&... item) { (decode_member(result, value, path, item), ...); }, Described<T>::fields.items);
        return result;
    } else if constexpr (kind == Kind::optional) {
        static_assert(dependent_false<T>,
                      "neomcp: std::optional is allowed only as the type of a member of a described struct");
    } else if constexpr (has_custom_schema<T>) {
        try {
            return value.get<T>();
        } catch (const json::exception& error) {
            fail(path, std::string("value accepted by the conversion of the type (") + error.what() + ")");
        }
    } else {
        static_assert(dependent_false<T>,
                      "neomcp: no neomcp::Schema for this type (describe it with neomcp::Described or "
                      "neomcp::DescribedEnum, or specialise neomcp::Schema)");
    }
}

template <class T>
json encode(const T& value);

// Writes one member of a described struct into `object`; a std::optional that holds nothing writes nothing.
template <class T, class F>
void encode_member(json& object, const T& value, const F& item) {
    static_assert(std::is_base_of_v<typename F::class_type, T>,
                  "neomcp: a member listed for a described struct belongs to an unrelated class");
    using Member = typename F::member_type;
    using Core = optional_core_t<Member>;
    static_assert(has_schema<Core>(), "neomcp: no neomcp::Schema for this member type");
    const Member& member = value.*(item.pointer);
    if constexpr (is_optional_v<Member>) {
        if (member.has_value()) {
            set_keyword(object, item.name, encode<Core>(*member));
        }
    } else {
        set_keyword(object, item.name, encode<Member>(member));
    }
}

template <class T>
json encode(const T& value) {
    constexpr Kind kind = kind_of<T>();
    if constexpr (kind == Kind::any_json) {
        return value;
    } else if constexpr (kind == Kind::boolean) {
        return json(value);
    } else if constexpr (kind == Kind::integer) {
        return number_json(value);
    } else if constexpr (kind == Kind::floating) {
        return json(static_cast<double>(value));
    } else if constexpr (kind == Kind::string) {
        return json(value);
    } else if constexpr (kind == Kind::array) {
        using Element = typename vector_traits<T>::element;
        json result = json::array();
        for (const auto& item : value) {
            result.push_back(encode<Element>(item));
        }
        return result;
    } else if constexpr (kind == Kind::dictionary) {
        using Element = typename string_map_traits<T>::element;
        json result = json::object();
        for (const auto& item : value) {
            set_keyword(result, item.first, encode<Element>(item.second));
        }
        return result;
    } else if constexpr (kind == Kind::enumeration) {
        for (const auto& entry : DescribedEnum<T>::values) {
            if (entry.value == value) {
                return json(std::string(entry.name));
            }
        }
        throw std::invalid_argument("neomcp: the enum value has no declared name");
    } else if constexpr (kind == Kind::object) {
        json result = json::object();
        std::apply([&](const auto&... item) { (encode_member(result, value, item), ...); }, Described<T>::fields.items);
        return result;
    } else if constexpr (kind == Kind::optional) {
        static_assert(dependent_false<T>,
                      "neomcp: std::optional is allowed only as the type of a member of a described struct");
    } else if constexpr (has_custom_schema<T>) {
        json result;
        neojson::adl_serializer<T>::to_json(result, value);
        return result;
    } else {
        static_assert(dependent_false<T>,
                      "neomcp: no neomcp::Schema for this type (describe it with neomcp::Described or "
                      "neomcp::DescribedEnum, or specialise neomcp::Schema)");
    }
}

}  // namespace detail

// Decodes `value` as a T. Throws DecodeError when the document does not have the shape of T.
template <class T>
T from_json(const json& value) {
    return detail::decode<T>(value, nullptr);
}

// Encodes `value` as JSON.
template <class T>
json to_json(const T& value) {
    return detail::encode<T>(value);
}

}  // namespace neomcp
