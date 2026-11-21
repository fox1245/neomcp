// The compiled form of a schema document, shared by the compiler and the evaluator.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "neomcp/json.h"
#include "neomcp/json_schema.h"
#include "schema/pattern.h"

namespace neomcp::json_schema::detail {

// The JSON types a `type` keyword names, as a set. "integer" is a subset of "number" and is tested separately.
namespace type_bit {
inline constexpr std::uint8_t null = 1;
inline constexpr std::uint8_t boolean = 2;
inline constexpr std::uint8_t object = 4;
inline constexpr std::uint8_t array = 8;
inline constexpr std::uint8_t number = 16;
inline constexpr std::uint8_t integer = 32;
inline constexpr std::uint8_t string = 64;
}  // namespace type_bit

struct Node;

struct PatternProperty {
    std::shared_ptr<const Pattern> pattern;
    const Node* schema = nullptr;
};

// One schema of the document, with every reference already linked to the node it points at. A node holds only the
// keywords that assert or apply; the others are annotations and are not kept.
struct Node {
    // Boolean schemas are `always` (true) and `never` (false).
    enum class Form : std::uint8_t { schema, always, never };

    Form form = Form::schema;
    // Where the schema sits in the document, as an RFC 6901 pointer.
    std::string pointer;

    const Node* ref = nullptr;

    // Validation keywords for any value.
    std::uint8_t types = 0;  // 0: no `type` keyword
    std::optional<json> const_value;
    std::optional<json::array_t> enum_values;

    // Numbers.
    std::optional<json> minimum;
    std::optional<json> maximum;
    std::optional<json> exclusive_minimum;
    std::optional<json> exclusive_maximum;
    std::optional<json> multiple_of;

    // Strings.
    std::optional<std::size_t> min_length;
    std::optional<std::size_t> max_length;
    std::shared_ptr<const Pattern> pattern;

    // Arrays.
    std::optional<std::size_t> min_items;
    std::optional<std::size_t> max_items;
    bool unique_items = false;
    std::vector<const Node*> prefix_items;
    const Node* items = nullptr;
    const Node* contains = nullptr;
    std::optional<std::size_t> min_contains;
    std::optional<std::size_t> max_contains;

    // Objects.
    std::optional<std::size_t> min_properties;
    std::optional<std::size_t> max_properties;
    std::vector<std::string> required;
    std::vector<std::pair<std::string, std::vector<std::string>>> dependent_required;
    std::map<std::string, const Node*, std::less<>> properties;
    std::vector<PatternProperty> pattern_properties;
    const Node* additional_properties = nullptr;
    const Node* property_names = nullptr;
    std::vector<std::pair<std::string, const Node*>> dependent_schemas;

    // Applicators that look at the same value.
    std::vector<const Node*> all_of;
    std::vector<const Node*> any_of;
    std::vector<const Node*> one_of;
    const Node* not_schema = nullptr;
    const Node* if_schema = nullptr;
    const Node* then_schema = nullptr;
    const Node* else_schema = nullptr;

    const Node* unevaluated_items = nullptr;
    const Node* unevaluated_properties = nullptr;
};

struct Compiled {
    Limits limits;
    const Node* root = nullptr;
    // Whether any schema has an `unevaluated*` keyword; only then does the evaluator track what was evaluated.
    bool track_annotations = false;
    // Deque: nodes point at each other, so their addresses must not change as more are added.
    std::deque<Node> nodes;
};

// Check and compile `schema`.
std::variant<std::shared_ptr<const Compiled>, Issue> compile_schema(const json& schema, const Limits& limits);

// Validate `value` against the root of `compiled`.
std::optional<Issue> evaluate(const Compiled& compiled, const json& value);

// Escape a member name for use as an RFC 6901 reference token.
std::string escape_token(std::string_view token);

// A value as JSON text for a message. Text that is not valid UTF-8 is printed with replacement characters instead of
// failing.
inline std::string dump_text(const json& value) {
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

// A string as a JSON string literal, for a message.
inline std::string quote(std::string_view text) { return dump_text(json(std::string(text))); }

}  // namespace neomcp::json_schema::detail
