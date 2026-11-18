// URI references as RFC 3986 defines them: splitting, resolving against a base, and turning back into text.
// Only what resolving `$id` and `$ref` needs; nothing is fetched and nothing is normalized beyond dot segments.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace neomcp::json_schema::detail {

struct Uri {
    std::optional<std::string> scheme;
    std::optional<std::string> authority;
    std::string path;
    std::optional<std::string> query;
    std::optional<std::string> fragment;
};

// Split a URI reference into its five parts. Every string has a split, so this cannot fail.
Uri parse_uri(std::string_view text);

// RFC 3986 section 5.2: the reference `ref` seen from the base `base`.
Uri resolve_uri(const Uri& base, const Uri& ref);

// RFC 3986 section 5.3.
std::string to_string(const Uri& uri);

// The URI without its fragment: the identity of the document or resource it points into.
std::string without_fragment(const Uri& uri);

// Replace each "%XX" with the byte it stands for. A '%' that does not start two hexadecimal digits stays.
std::string percent_decode(std::string_view text);

}  // namespace neomcp::json_schema::detail
