// The parameters of a tool that a client mirrors into HTTP headers.
//
// A revision that defines argument headers (see profile::ArgumentHeaders) lets the input schema of a tool mark a
// primitive parameter with an extension keyword whose value is the name portion of a header. A client then copies
// the argument into that header, and the server checks the header against the request body.
//
// `header_parameters` reads the marks of one input schema and checks every rule a mark must obey. A server applies it
// when a tool is registered (Server does), because a mark that breaks a rule makes the tool definition invalid and
// clients drop such a tool; the session applies it again to every tool call it serves, so that a tool of a host that
// did not check is still served safely.
//
// The range of an integer is a rule about values, not about the schema: a schema need not declare bounds for an
// integer parameter. The range is enforced on the values of calls: a call whose argument is an integer outside
// +-max_safe_integer (or a number that is not whole) cannot be mirrored into a header and is refused as a header
// mismatch.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "neomcp/json.h"
#include "neomcp/profile.h"

namespace neomcp {

// The type of a parameter that may be mirrored.
enum class HeaderParameterType { integer, string, boolean };

// The integers a header can carry: the range in which a double-precision number holds every integer exactly,
// -(2^53 - 1) to 2^53 - 1.
inline constexpr std::int64_t max_safe_integer = 9007199254740991;

// One marked parameter.
struct HeaderParameter {
    // The name portion the mark gives, as the schema spells it. Names are unique without regard to case.
    std::string name;
    // The names of the properties that lead from the root of the schema to the parameter; never empty.
    std::vector<std::string> path;
    HeaderParameterType type = HeaderParameterType::string;
    // RFC 6901 JSON Pointer to the schema of the parameter, for messages.
    std::string pointer;
};

// Why a schema was refused.
struct HeaderMarkIssue {
    // RFC 6901 JSON Pointer to the place in the schema where the rule is broken ("" is the whole schema).
    std::string pointer;
    // The rule, in words.
    std::string rule;
};

// The deepest nesting of schemas that `header_parameters` follows by default.
inline constexpr std::size_t default_header_schema_depth = 64;

// The parameters that the keyword `keyword` marks in `input_schema`, in the order of their paths, or the first rule a
// mark breaks. A mark must
//   - have a string for its value that is a non-empty HTTP token, unique among the marks without regard to case;
//   - sit on a property reached from the root through `properties` members only: not on the root, and not below
//     `items`, `prefixItems`, a composition (`allOf`, `anyOf`, `oneOf`, `not`), a condition (`if`, `then`, `else`),
//     `$ref` or any other keyword;
//   - sit on a property whose `type` is integer, string or boolean (one of them, with `"null"` allowed besides), so
//     not number;
// The values of `const`, `enum`, `default` and `examples` are data, and so are the names of properties: a property
// that is called like the keyword is not a mark. Any other keyword is searched, so a mark below an unknown keyword is
// refused rather than overlooked. A schema nested deeper than `max_depth` is refused.
std::variant<std::vector<HeaderParameter>, HeaderMarkIssue> header_parameters(
    const json& input_schema, std::string_view keyword, std::size_t max_depth = default_header_schema_depth);

// Why a value cannot be mirrored into a header.
struct HeaderValueIssue {
    std::string rule;  // in words; never holds the value
};

// The text of a string for a header value, in the way the revision's framing (`encoded_header_value`) prescribes: the
// string as it is when it is plain, otherwise the framing's prefix, the Base64 of its bytes (standard alphabet, with
// padding) and its suffix. A string is plain when every byte is visible ASCII or a space (0x20 to 0x7E), it does not
// begin or end with a space, and it does not both begin with the prefix and end with the suffix (the form that would be
// read as encoded). This is stricter than what a receiver accepts as plain, which also holds a horizontal tab and
// any space inside: a string with a tab is always encoded, so that what is written is valid in either reading.
std::string encode_header_text(std::string_view text, const profile::EncodedHeaderValue& framing);

// The value of the header that mirrors a parameter: a string by encode_header_text, an integer in decimal (a whole
// number within +-max_safe_integer; one outside is an issue), a boolean as `true` or `false`. A null value has no
// header and gives an empty result; any other kind of value is an issue.
std::variant<std::optional<std::string>, HeaderValueIssue> encode_header_value(const json& value,
                                                                              const profile::EncodedHeaderValue& framing);

}  // namespace neomcp
