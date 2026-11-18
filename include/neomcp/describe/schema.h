// The JSON Schema (2020-12 keywords) of a C++ type.
//
// Built in:
//   bool                                   {"type":"boolean"}
//   integer types                          {"type":"integer"} with the type's range when it is narrower than 64 bits
//                                          (and minimum 0 for unsigned 64-bit); character types have no schema
//   float, double                          {"type":"number"}
//   std::string                            {"type":"string"}
//   std::vector<T>                         {"type":"array","items":<T>}
//   std::map<std::string, T>               {"type":"object","additionalProperties":<T>}
//   a described enum                       {"type":"string","enum":[<names in declaration order>]}
//   a described struct                     {"type":"object","properties":{...},"required":[...],
//                                           "additionalProperties":false}, members in declaration order,
//                                           `required` left out when every member is optional
//   neomcp::json                           {} (any value)
//
// A std::optional<T> member has the schema of T and is not required; std::optional is accepted nowhere else.
// A type of your own gets its schema by specialising Schema:
//
//   template <> struct neomcp::Schema<Celsius> {
//       static neomcp::json make() { return neomcp::json::parse(R"({"type":"number","minimum":-273.15})"); }
//   };
//
// A Schema specialisation takes precedence over everything above for its type. Types that refer to themselves
// (a node that holds a vector of nodes) have no finite schema without references and need a Schema specialisation.
#pragma once

#include <concepts>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "neomcp/describe/model.h"
#include "neomcp/json.h"

namespace neomcp {

// Specialise to give a type of your own its schema. `make` returns the schema document.
template <class T>
struct Schema {};

namespace detail {

template <class T>
concept has_custom_schema = requires {
    { Schema<T>::make() } -> std::convertible_to<json>;
};

template <class T>
inline constexpr bool is_character_type_v = std::is_same_v<T, char> || std::is_same_v<T, wchar_t> ||
                                            std::is_same_v<T, char8_t> || std::is_same_v<T, char16_t> ||
                                            std::is_same_v<T, char32_t>;

// Integer types are the integral types except bool and the character types.
template <class T>
inline constexpr bool is_integer_type_v = std::is_integral_v<T> && !std::is_same_v<T, bool> && !is_character_type_v<T>;

template <class T>
inline constexpr bool is_floating_type_v = std::is_same_v<T, float> || std::is_same_v<T, double>;

template <class T>
struct vector_traits {
    static constexpr bool value = false;
};
template <class T, class A>
struct vector_traits<std::vector<T, A>> {
    static constexpr bool value = true;
    using element = T;
};

template <class T>
struct string_map_traits {
    static constexpr bool value = false;
};
template <class T, class C, class A>
struct string_map_traits<std::map<std::string, T, C, A>> {
    static constexpr bool value = true;
    using element = T;
};

// How the library handles a type, decided by the type alone.
enum class Kind {
    any_json,
    boolean,
    integer,
    floating,
    string,
    array,
    dictionary,
    enumeration,
    object,
    optional,
    unsupported,
};

template <class T>
constexpr Kind kind_of() {
    if constexpr (std::is_same_v<T, json>) {
        return Kind::any_json;
    } else if constexpr (std::is_same_v<T, bool>) {
        return Kind::boolean;
    } else if constexpr (is_integer_type_v<T>) {
        return Kind::integer;
    } else if constexpr (is_floating_type_v<T>) {
        return Kind::floating;
    } else if constexpr (std::is_same_v<T, std::string>) {
        return Kind::string;
    } else if constexpr (vector_traits<T>::value) {
        return Kind::array;
    } else if constexpr (string_map_traits<T>::value) {
        return Kind::dictionary;
    } else if constexpr (described_enum<T>) {
        return Kind::enumeration;
    } else if constexpr (described<T>) {
        return Kind::object;
    } else if constexpr (is_optional_v<T>) {
        return Kind::optional;
    } else {
        return Kind::unsupported;
    }
}

// True when a value of the type can be described and converted. A described struct is not looked into: its own
// members are checked where its schema is made.
template <class T>
constexpr bool has_schema() {
    if constexpr (has_custom_schema<T>) {
        return true;
    } else if constexpr (kind_of<T>() == Kind::array) {
        return has_schema<typename vector_traits<T>::element>();
    } else if constexpr (kind_of<T>() == Kind::dictionary) {
        return has_schema<typename string_map_traits<T>::element>();
    } else {
        return kind_of<T>() != Kind::unsupported && kind_of<T>() != Kind::optional;
    }
}

template <class T>
json integer_schema() {
    json schema = json::object();
    set_keyword(schema, "type", json("integer"));
    if constexpr (sizeof(T) < 8) {
        set_keyword(schema, "minimum", number_json(std::numeric_limits<T>::min()));
        set_keyword(schema, "maximum", number_json(std::numeric_limits<T>::max()));
    } else if constexpr (std::is_unsigned_v<T>) {
        set_keyword(schema, "minimum", number_json(std::uint64_t{0}));
    }
    return schema;
}

template <class T>
json enum_schema() {
    static_assert(unique_enum_names(DescribedEnum<T>::values), "neomcp: two enumerators of a described enum have the same name");
    json names = json::array();
    for (const auto& entry : DescribedEnum<T>::values) {
        names.push_back(json(std::string(entry.name)));
    }
    json schema = json::object();
    set_keyword(schema, "type", json("string"));
    set_keyword(schema, "enum", std::move(names));
    return schema;
}

}  // namespace detail

template <class T>
json schema_of();

namespace detail {

// The schema of one member, with its description and options applied, added to `properties` (and to `required`
// unless the member is a std::optional).
template <class T, class F>
void add_property(json& properties, json& required, const F& item) {
    static_assert(std::is_base_of_v<typename F::class_type, T>,
                  "neomcp: a member listed for a described struct belongs to an unrelated class");
    using Member = typename F::member_type;
    using Core = optional_core_t<Member>;
    static_assert(has_schema<Core>(),
                  "neomcp: no neomcp::Schema for this member type (describe it with neomcp::Described or "
                  "neomcp::DescribedEnum, or specialise neomcp::Schema; std::optional is allowed only as the member's own type)");
    json schema = schema_of<Core>();
    if (schema.is_boolean() && schema.get<bool>()) {
        schema = json::object();  // the schema `true` accepts everything, which is the empty object
    }
    if (schema.is_object()) {
        if (!item.description.empty()) {
            set_keyword(schema, "description", json(std::string(item.description)));
        }
        std::apply([&](const auto&... option) { (option.apply(schema), ...); }, item.options);
    }
    set_keyword(properties, item.name, std::move(schema));
    if constexpr (!is_optional_v<Member>) {
        required.push_back(json(std::string(item.name)));
    }
}

template <class T>
json object_schema() {
    static_assert(unique_names(Described<T>::fields), "neomcp: two members of a described struct have the same name");
    json properties = json::object();
    json required = json::array();
    std::apply([&](const auto&... item) { (add_property<T>(properties, required, item), ...); }, Described<T>::fields.items);
    json schema = json::object();
    set_keyword(schema, "type", json("object"));
    set_keyword(schema, "properties", std::move(properties));
    if (!required.empty()) {
        set_keyword(schema, "required", std::move(required));
    }
    set_keyword(schema, "additionalProperties", json(false));
    return schema;
}

}  // namespace detail

// The JSON Schema of T (see the top of this file). The same type always gives the same document, key for key.
template <class T>
json schema_of() {
    static_assert(!detail::is_optional_v<T>,
                  "neomcp: std::optional has no schema of its own; it is allowed only as the type of a member of a described struct");
    if constexpr (detail::has_custom_schema<T>) {
        return Schema<T>::make();
    } else {
        constexpr detail::Kind kind = detail::kind_of<T>();
        json schema = json::object();
        if constexpr (kind == detail::Kind::any_json) {
            return schema;
        } else if constexpr (kind == detail::Kind::boolean) {
            detail::set_keyword(schema, "type", json("boolean"));
            return schema;
        } else if constexpr (kind == detail::Kind::integer) {
            return detail::integer_schema<T>();
        } else if constexpr (kind == detail::Kind::floating) {
            detail::set_keyword(schema, "type", json("number"));
            return schema;
        } else if constexpr (kind == detail::Kind::string) {
            detail::set_keyword(schema, "type", json("string"));
            return schema;
        } else if constexpr (kind == detail::Kind::array) {
            detail::set_keyword(schema, "type", json("array"));
            detail::set_keyword(schema, "items", schema_of<typename detail::vector_traits<T>::element>());
            return schema;
        } else if constexpr (kind == detail::Kind::dictionary) {
            detail::set_keyword(schema, "type", json("object"));
            detail::set_keyword(schema, "additionalProperties", schema_of<typename detail::string_map_traits<T>::element>());
            return schema;
        } else if constexpr (kind == detail::Kind::enumeration) {
            return detail::enum_schema<T>();
        } else if constexpr (kind == detail::Kind::object) {
            return detail::object_schema<T>();
        } else {
            static_assert(detail::dependent_false<T>,
                          "neomcp: no neomcp::Schema for this type (describe it with neomcp::Described or "
                          "neomcp::DescribedEnum, or specialise neomcp::Schema)");
            return schema;
        }
    }
}

}  // namespace neomcp
