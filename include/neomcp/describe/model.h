// The description of a type: which members a struct has and how they are named, which names an enum has, and the
// options that narrow what a member accepts. Everything here can be used in a constant expression.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

#include "neomcp/json.h"

namespace neomcp {

// Specialise this to describe a struct:
//
//   template <> struct neomcp::Described<AddArgs> {
//       static constexpr auto fields = neomcp::fields(
//           neomcp::field(&AddArgs::a, "a", "first addend"),
//           neomcp::field(&AddArgs::b, "b", "second addend", neomcp::minimum(0)));
//   };
//
// NEOMCP_DESCRIBE writes this specialisation for the common case. The members appear in the schema and in encoded
// objects in the order they are listed.
template <class T>
struct Described {};

// One enumerator of a described enum: the value and the name it has in JSON.
template <class E>
struct EnumEntry {
    E value;
    std::string_view name;
};

// Specialise this to describe an enum as a string enum. `values` is an array (or std::array) of EnumEntry<E>:
//
//   template <> struct neomcp::DescribedEnum<Color> {
//       static constexpr neomcp::EnumEntry<Color> values[] = {{Color::red, "red"}, {Color::green, "green"}};
//   };
//
// NEOMCP_DESCRIBE_ENUM writes this specialisation with the enumerators' own names.
template <class E>
struct DescribedEnum {};

template <class T>
concept described = requires { Described<T>::fields; };

template <class E>
concept described_enum = std::is_enum_v<E> && requires { DescribedEnum<E>::values; };

namespace detail {

template <class T>
inline constexpr bool dependent_false = false;

template <class T>
struct is_optional : std::false_type {};
template <class T>
struct is_optional<std::optional<T>> : std::true_type {};
template <class T>
inline constexpr bool is_optional_v = is_optional<T>::value;

// The type of the value a member holds: std::optional<T> holds a T.
template <class T>
struct optional_core {
    using type = T;
};
template <class T>
struct optional_core<std::optional<T>> {
    using type = T;
};
template <class T>
using optional_core_t = typename optional_core<T>::type;

// A number that an option may carry: not bool, and not a character type.
template <class V>
inline constexpr bool is_number_v = std::is_arithmetic_v<V> && !std::is_same_v<V, bool> && !std::is_same_v<V, char> &&
                                    !std::is_same_v<V, wchar_t> && !std::is_same_v<V, char8_t> &&
                                    !std::is_same_v<V, char16_t> && !std::is_same_v<V, char32_t>;

// What an option stores for a value written in a field list: string literals become views, numbers keep their type.
template <class V>
inline constexpr bool is_text_v = std::is_same_v<std::decay_t<V>, const char*> || std::is_same_v<std::decay_t<V>, char*> ||
                                  std::is_same_v<std::decay_t<V>, std::string_view>;
template <class V>
using option_value_t = std::conditional_t<is_text_v<V>, std::string_view, std::decay_t<V>>;

template <class V>
json number_json(V value) {
    if constexpr (std::is_floating_point_v<V>) {
        return json(static_cast<double>(value));
    } else if constexpr (std::is_signed_v<V>) {
        return json(static_cast<std::int64_t>(value));
    } else {
        return json(static_cast<std::uint64_t>(value));
    }
}

// True when a number can be written to JSON as it is: an integer always can; a floating point number only when it is
// neither an infinity nor a NaN once it is the double that number_json writes (JSON has no spelling for them, and a
// writer would turn them into null).
template <class V>
constexpr bool is_finite_number(V value) {
    if constexpr (std::is_floating_point_v<V>) {
        const double wide = static_cast<double>(value);
        return wide >= std::numeric_limits<double>::lowest() && wide <= (std::numeric_limits<double>::max)();
    } else {
        static_cast<void>(value);
        return true;
    }
}

// Called by the function that makes an option from a number it was given. Evaluated as a constant (the field list of a
// Described specialisation is), a number that is not finite stops the compilation here; at run time nothing happens,
// and the schema derivation (schema_of) refuses the option, naming the member.
constexpr void refuse_when_constant(bool finite, const char* message) {
    if (!finite && std::is_constant_evaluated()) {
        throw std::invalid_argument(message);
    }
}

// The JSON form of a value an option carries.
template <class V>
json option_json(const V& value) {
    if constexpr (std::is_same_v<V, bool>) {
        return json(value);
    } else if constexpr (std::is_same_v<V, std::string_view>) {
        return json(std::string(value));
    } else {
        return number_json(value);
    }
}

inline void set_keyword(json& schema, std::string_view keyword, json value) {
    schema[std::string(keyword)] = std::move(value);
}

}  // namespace detail

// ---------------------------------------------------------------------------------------------------------------
// Field options. They add keywords to the member's schema and replace a keyword of the same name that the member's
// type already gives. They document and constrain the schema only: decoding does not enforce them (validating a
// value against the schema does).
//
// A number an option carries must be finite: JSON has no infinity and no NaN. An option made from one in a constant
// expression (as the field list of a Described specialisation is) does not compile; one made at run time is refused
// by schema_of with std::invalid_argument, which names the member and the option.

template <class V>
struct Minimum {
    V value;
    static constexpr std::string_view keyword = "neomcp::minimum";
    constexpr bool finite() const { return detail::is_finite_number(value); }
    template <class Member>
    static constexpr void check() {
        static_assert(detail::is_number_v<Member>, "neomcp::minimum applies to integer and floating point members only");
    }
    void apply(json& schema) const { detail::set_keyword(schema, "minimum", detail::number_json(value)); }
};

template <class V>
struct Maximum {
    V value;
    static constexpr std::string_view keyword = "neomcp::maximum";
    constexpr bool finite() const { return detail::is_finite_number(value); }
    template <class Member>
    static constexpr void check() {
        static_assert(detail::is_number_v<Member>, "neomcp::maximum applies to integer and floating point members only");
    }
    void apply(json& schema) const { detail::set_keyword(schema, "maximum", detail::number_json(value)); }
};

struct MinLength {
    std::size_t value;
    template <class Member>
    static constexpr void check() {
        static_assert(std::is_same_v<Member, std::string>, "neomcp::min_length applies to std::string members only");
    }
    void apply(json& schema) const { detail::set_keyword(schema, "minLength", detail::number_json(value)); }
};

struct MaxLength {
    std::size_t value;
    template <class Member>
    static constexpr void check() {
        static_assert(std::is_same_v<Member, std::string>, "neomcp::max_length applies to std::string members only");
    }
    void apply(json& schema) const { detail::set_keyword(schema, "maxLength", detail::number_json(value)); }
};

namespace detail {
template <class T>
struct is_vector : std::false_type {};
template <class T, class A>
struct is_vector<std::vector<T, A>> : std::true_type {};
}  // namespace detail

struct MinItems {
    std::size_t value;
    template <class Member>
    static constexpr void check() {
        static_assert(detail::is_vector<Member>::value, "neomcp::min_items applies to std::vector members only");
    }
    void apply(json& schema) const { detail::set_keyword(schema, "minItems", detail::number_json(value)); }
};

struct MaxItems {
    std::size_t value;
    template <class Member>
    static constexpr void check() {
        static_assert(detail::is_vector<Member>::value, "neomcp::max_items applies to std::vector members only");
    }
    void apply(json& schema) const { detail::set_keyword(schema, "maxItems", detail::number_json(value)); }
};

// The allowed values: all strings (for a std::string or described enum member) or all numbers (for a number member).
template <class V, std::size_t N>
struct OneOf {
    std::array<V, N> values;
    static constexpr std::string_view keyword = "neomcp::one_of";
    constexpr bool finite() const {
        for (const V& value : values) {
            if (!detail::is_finite_number(value)) return false;
        }
        return true;
    }
    template <class Member>
    static constexpr void check() {
        if constexpr (std::is_same_v<V, std::string_view>) {
            static_assert(std::is_same_v<Member, std::string> || described_enum<Member>,
                          "neomcp::one_of with strings applies to std::string and described enum members only");
        } else {
            static_assert(detail::is_number_v<Member>, "neomcp::one_of with numbers applies to integer and floating point members only");
        }
    }
    void apply(json& schema) const {
        json list = json::array();
        for (const V& value : values) {
            list.push_back(detail::option_json(value));
        }
        detail::set_keyword(schema, "enum", std::move(list));
    }
};

// Documentation: adds `default` to the schema. Nothing applies it, and decoding does not fill it in.
template <class V>
struct DefaultValue {
    V value;
    static constexpr std::string_view keyword = "neomcp::default_value";
    constexpr bool finite() const { return detail::is_finite_number(value); }
    template <class Member>
    static constexpr void check() {
        if constexpr (std::is_same_v<V, bool>) {
            static_assert(std::is_same_v<Member, bool>, "neomcp::default_value with a bool applies to bool members only");
        } else if constexpr (std::is_same_v<V, std::string_view>) {
            static_assert(std::is_same_v<Member, std::string> || described_enum<Member>,
                          "neomcp::default_value with a string applies to std::string and described enum members only");
        } else if constexpr (detail::is_number_v<V>) {
            static_assert(detail::is_number_v<Member>, "neomcp::default_value with a number applies to integer and floating point members only");
        } else {
            static_assert(detail::dependent_false<V>,
                          "neomcp::default_value takes a bool, a number, a string literal or a std::string_view");
        }
    }
    void apply(json& schema) const { detail::set_keyword(schema, "default", detail::option_json(value)); }
};

// Documentation: adds `title`, a short display name of the member (a form shows it as the label of the field).
struct Title {
    std::string_view text;
    template <class Member>
    static constexpr void check() {}
    void apply(json& schema) const { detail::set_keyword(schema, "title", json(std::string(text))); }
};

// Marks the parameter to be mirrored into an HTTP header whose name portion is `name`, where a revision that the
// server serves lets a client do that. The header is then "<prefix><name>"; the prefix and the schema keyword that
// carries the mark come from the revision's profile, so nothing here spells them. The member's own schema stays as
// it is: Server::tool<Args> writes the keyword into the tool's input schema when it registers the tool, once for
// every keyword the served revisions define, and writes nothing when none does.
//
// Applies to bool, integer and std::string members, also inside a described struct that is itself a member (not
// below a vector or a map). The rules for a mark are checked when the tool is registered (see header_parameters in
// neomcp/argument_headers.h): the name must be an HTTP token and unique among the marks without regard to case. An
// integer member needs no bounds; a call whose value is outside +-9007199254740991 is refused when it is made. The
// option is not enforced by decoding, and a schema made by schema_of<Args>() alone does not carry the mark.
struct HeaderName {
    std::string_view name;
    template <class Member>
    static constexpr void check() {
        static_assert(std::is_same_v<Member, bool> || std::is_same_v<Member, std::string> ||
                          (std::is_integral_v<Member> && detail::is_number_v<Member>),
                      "neomcp::mirror_in_header applies to bool, integer and std::string members only");
    }
    void apply(json&) const {}
};

// One parameter marked with neomcp::mirror_in_header: the names of the members that lead to it from the root of the
// arguments, and the name portion of its header.
struct HeaderMark {
    std::vector<std::string> path;
    std::string name;
};

template <class V>
constexpr Minimum<std::decay_t<V>> minimum(V value) {
    static_assert(detail::is_number_v<V>, "neomcp::minimum takes an integer or a floating point number");
    detail::refuse_when_constant(detail::is_finite_number(value), "neomcp::minimum takes a finite number");
    return {value};
}

template <class V>
constexpr Maximum<std::decay_t<V>> maximum(V value) {
    static_assert(detail::is_number_v<V>, "neomcp::maximum takes an integer or a floating point number");
    detail::refuse_when_constant(detail::is_finite_number(value), "neomcp::maximum takes a finite number");
    return {value};
}

constexpr MinLength min_length(std::size_t count) { return {count}; }
constexpr MaxLength max_length(std::size_t count) { return {count}; }
constexpr MinItems min_items(std::size_t count) { return {count}; }
constexpr MaxItems max_items(std::size_t count) { return {count}; }

template <class... Vs>
constexpr auto one_of(Vs... values) {
    static_assert(sizeof...(Vs) > 0, "neomcp::one_of needs at least one value");
    constexpr bool all_text = (detail::is_text_v<Vs> && ...);
    constexpr bool all_numbers = (detail::is_number_v<Vs> && ...);
    static_assert(all_text || all_numbers, "neomcp::one_of takes only string literals or only numbers");
    if constexpr (all_text) {
        return OneOf<std::string_view, sizeof...(Vs)>{{std::string_view(values)...}};
    } else if constexpr (all_numbers) {
        using Common = std::common_type_t<Vs...>;
        OneOf<Common, sizeof...(Vs)> option{{static_cast<Common>(values)...}};
        detail::refuse_when_constant(option.finite(), "neomcp::one_of takes finite numbers");
        return option;
    } else {
        return OneOf<int, 0>{};
    }
}

constexpr HeaderName mirror_in_header(std::string_view name) { return {name}; }

constexpr Title title(std::string_view text) { return {text}; }

template <class V>
constexpr DefaultValue<detail::option_value_t<V>> default_value(V value) {
    DefaultValue<detail::option_value_t<V>> option{static_cast<detail::option_value_t<V>>(value)};
    detail::refuse_when_constant(option.finite(), "neomcp::default_value takes a finite number");
    return option;
}

// ---------------------------------------------------------------------------------------------------------------
// Fields

// A member of a struct: where it is, the name and description it has in the schema, and its options.
template <class Class, class Member, class... Options>
struct Field {
    using class_type = Class;
    using member_type = Member;

    Member Class::* pointer;
    std::string_view name;
    std::string_view description;
    std::tuple<Options...> options;
};

// Describes one member. `name` is the JSON name; an empty `description` leaves `description` out of the schema.
// An option that does not fit the member's type is a compile error.
template <class Class, class Member, class... Options>
constexpr Field<Class, Member, Options...> field(Member Class::* pointer, std::string_view name,
                                                 std::string_view description, Options... options) {
    static_assert(!std::is_function_v<Member>, "neomcp::field describes data members, not member functions");
    static_assert(!std::is_const_v<Member>, "neomcp::field cannot describe a const member: decoding has to assign it");
    static_assert(!detail::is_optional_v<detail::optional_core_t<Member>>,
                  "neomcp::field: std::optional<std::optional<T>> is not supported");
    (Options::template check<detail::optional_core_t<Member>>(), ...);
    return {pointer, name, description, std::tuple<Options...>(options...)};
}

// The members of a struct, in order.
template <class... Fields>
struct FieldList {
    static constexpr std::size_t size = sizeof...(Fields);
    std::tuple<Fields...> items;
};

template <class... Fields>
constexpr FieldList<Fields...> fields(Fields... list) {
    return {std::tuple<Fields...>(list...)};
}

namespace detail {

// Named by NEOMCP_DESCRIBE when a member is not followed by its description.
template <class T>
constexpr int describe_needs_member_description_pairs() {
    static_assert(dependent_false<T>, "NEOMCP_DESCRIBE takes each member followed by its description (use \"\" for none)");
    return 0;
}

// True when no two members of the list have the same JSON name.
template <class... Fields>
constexpr bool unique_names(const FieldList<Fields...>& list) {
    std::array<std::string_view, sizeof...(Fields)> names = std::apply(
        [](const auto&... item) { return std::array<std::string_view, sizeof...(Fields)>{item.name...}; }, list.items);
    for (std::size_t i = 0; i < names.size(); ++i) {
        for (std::size_t j = i + 1; j < names.size(); ++j) {
            if (names[i] == names[j]) {
                return false;
            }
        }
    }
    return true;
}

// True when no two enumerators of the table have the same name.
template <class Entries>
constexpr bool unique_enum_names(const Entries& entries) {
    const std::size_t count = std::size(entries);
    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t j = i + 1; j < count; ++j) {
            if (entries[i].name == entries[j].name) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace detail

}  // namespace neomcp
