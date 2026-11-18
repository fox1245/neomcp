// A scan of a regular expression's text for the constructs that newer ECMAScript has and that the grammar behind
// std::regex does not.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace neomcp::json_schema::detail {

// std::regex implements the ECMAScript grammar of ECMA-262 3rd edition (C++ [re.synopt]). Its atoms are a pattern
// character, '.', an atom escape, a character class, "(...)" and "(?:...)"; its assertions are '^', '$', "\b", "\B",
// "(?=...)" and "(?!...)"; its escapes are "\uXXXX" with four hexadecimal digits, "\xXX", "\cX", the class escapes
// and identity escapes. Four things that later editions added are not in it, and whether an engine rejects them or
// reads them as something else differs between standard libraries, so they are refused here, by the same answer on
// every platform: Unicode property escapes ("\p{..}", "\P{..}"), code point escapes ("\u{...}"), lookbehind
// assertions ("(?<=", "(?<!") and named groups ("(?<name>") with their back-references ("\k<name>").
//
// Returns the name of the first such construct in `pattern`, or nothing. Escapes and character classes are read as
// ECMAScript reads them, so "\\p{" (an escaped backslash) and "[(?<x]" (a class) contain none.
std::optional<std::string> find_unsupported_construct(std::string_view pattern);

}  // namespace neomcp::json_schema::detail
