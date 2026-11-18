// JSON Schema 2020-12 validation.
//
// A schema is checked and compiled once by `Validator::compile`; the resulting Validator is immutable, cheap to
// copy and safe to use from several threads. Every refusal and every failed validation is an Issue that names its
// place in the schema and in the value with RFC 6901 JSON Pointers; neither function throws.
//
// Supported: the core, applicator, unevaluated and validation vocabularies of 2020-12, boolean schemas, and local
// `$ref` (JSON Pointer fragments, `$anchor` names and `$id` resources inside the same document, with relative
// references resolved by RFC 3986). `format`, the content keywords and the meta-data keywords are annotations, as
// 2020-12 specifies by default; so is every keyword the dialect does not define.
//
// Refused by `compile`, with the pointer of the offending keyword: a `$schema` that names another dialect,
// `$dynamicRef`, `$recursiveRef`, a reference to another document, a reference that cannot be resolved, a cycle of
// references that makes no progress through the value, a `pattern` or `patternProperties` key that `std::regex`
// (ECMAScript grammar) cannot compile, a keyword of the wrong shape, and a schema nested deeper than
// Limits::max_schema_depth. A reference is never fetched from the network or the file system.
//
// `pattern` and `patternProperties` use `std::regex`, which works on the bytes of a string, not on code points.
#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "neomcp/json.h"

namespace neomcp::json_schema {

// Every bound the validator applies. A bound that is exceeded while validating ends the validation with an Issue; it
// never makes a subschema merely "not match", so `not`, `anyOf` and `if` cannot turn it into a pass.
struct Limits {
    // How many schemas may be nested while one value is checked: every applicator, every step into a member or an
    // item, and every followed reference counts one level, and the root schema is level 0. A level costs less than 2
    // KiB of native stack in a Debug build (measured with Clang 19), so the default stays near a quarter of the 512
    // KiB that some platforms give a thread. A value 40 levels deep checked against a recursive schema uses 80.
    std::size_t max_depth = 128;

    // How many levels of subschemas the compiler follows inside the schema document, counted from the schema being
    // compiled. Real-world schemas stay well below 32 levels.
    std::size_t max_schema_depth = 64;

    // How many levels of arrays and objects `const`, `enum` and `uniqueItems` compare or hash. Same reasoning as
    // max_depth.
    std::size_t max_value_depth = 256;

    // The longest string, in bytes, that a `pattern` or a `patternProperties` key is matched against. A longer
    // string ends the validation with an Issue instead of being handed to the regex engine, which matches
    // recursively: libstdc++ spends about 1 KiB of stack on each character of input matched by a repeated group
    // (measured with GCC 13), and its matcher crashes where the stack runs out. The default is the length at which
    // that stays below half of a 512 KiB thread stack. Patterns for names, identifiers and codes fit; raise it only
    // for a stack known to be larger.
    std::size_t max_pattern_input = 256;

    // How many schemas one `validate` call may evaluate in total. This bounds the work of a schema whose references
    // fan out (each level evaluates two subschemas that each evaluate two more) and that no depth limit stops.
    std::size_t max_evaluations = 10'000'000;
};

// Why a schema was refused or a value was rejected.
struct Issue {
    // RFC 6901 JSON Pointer into the validated value ("" is the root). Empty for a refused schema.
    std::string instance_pointer;
    // RFC 6901 JSON Pointer into the schema document, to the keyword that failed or is unsupported. A failure
    // found through a `$ref` points at the keyword in the referenced schema.
    std::string schema_pointer;
    std::string message;
};

namespace detail {
struct Compiled;
}  // namespace detail

// An immutable, compiled schema.
class Validator {
public:
    // Check `schema` and compile it, or say what is wrong with it. `limits` apply to the compiler and are kept for
    // every validation by the result.
    static std::variant<Validator, Issue> compile(const json& schema, Limits limits = {});

    // The first failure of `value` against the schema, or nothing when it is valid.
    std::optional<Issue> validate(const json& value) const;

private:
    explicit Validator(std::shared_ptr<const detail::Compiled> compiled) noexcept;
    std::shared_ptr<const detail::Compiled> compiled_;
};

}  // namespace neomcp::json_schema
