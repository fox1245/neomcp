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
// references that makes no progress through the value, a `pattern` or `patternProperties` key that cannot be
// matched exactly (below), a keyword of the wrong shape, and a schema nested deeper than Limits::max_schema_depth. A
// reference is never fetched from the network or the file system.
//
// Patterns. `pattern` and `patternProperties` are regular expressions in the dialect of ECMA-262 with the `u` flag
// (Unicode mode), which is what JSON Schema asks for: a pattern and a string are sequences of code points, a pattern
// is searched for in the string and not anchored, `.` does not match a line terminator, `\d`, `\w` and `\s` have
// their ECMA-262 meanings, and a pattern that is a syntax error in Unicode mode (an unescaped `]`, `{` or `}`, an
// escape of a character that is not a syntax character, a reversed range) is refused. Strings are read as UTF-8, and
// validation ends with an Issue at a string or property name that is not well-formed UTF-8.
//
// A pattern is compiled once, when the schema is compiled, and a string is checked in time proportional to its
// length, whatever the pattern: there is no backtracking. The engine behind this is RE2, and the pattern is
// translated to its syntax first. What cannot be translated with the same meaning is refused by `compile`, with the
// pointer of the keyword and a message that names the construct and its byte offset: lookahead and lookbehind
// assertions, back-references (numbered and named), group modifiers, Unicode properties other than the general
// categories (`\p{Lu}`, `\p{Letter}` and `\p{gc=Nd}` work; `C`, `Cn` and `LC` do not, because RE2 defines them
// differently), repeat counts above 1000, and any pattern that needs more memory than Limits::max_pattern_memory.
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
    // string ends the validation with an Issue instead of being checked. Matching takes time proportional to the
    // string, so this bounds the work one check does for a string a client chooses; patterns for names, identifiers
    // and codes fit the default.
    std::size_t max_pattern_input = 256;

    // The longest regular expression, in bytes, that `compile` accepts in a `pattern` or as a `patternProperties` key.
    // It bounds the work of translating and compiling a pattern, and the size of the program that results. The
    // default is longer than any pattern that describes a name, an identifier or a code.
    std::size_t max_pattern_length = 512;

    // The memory, in bytes, that the program of one compiled pattern may take, and that the engine may spend on the
    // tables it builds while it matches. A pattern whose program does not fit is refused by `compile`, with the
    // pointer of its keyword. (Something like `\p{L}{1000}` repeats a large class a thousand times.) Matching that
    // outgrows the part for its tables continues more slowly in the same time bound; it does not fail. The default
    // is the engine's own: 8 MiB.
    std::size_t max_pattern_memory = 8 * 1024 * 1024;

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
