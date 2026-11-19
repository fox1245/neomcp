// The translation of a `pattern` from the regular expression dialect of JSON Schema to the syntax of RE2, or the
// refusal of the pattern.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <variant>

namespace neomcp::json_schema::detail {

// A construct of a pattern that cannot be matched exactly, and where it begins.
struct PatternRefusal {
    std::size_t position = 0;  // byte offset into the pattern
    std::string construct;     // what is there, for a message: "a lookahead assertion (?=...)"
};

// JSON Schema patterns are ECMA-262 regular expressions, which an implementation should read with the `u` flag
// (Unicode mode: the pattern and the text are sequences of code points). RE2 reads another syntax and has a few
// meanings of its own, so a pattern is never handed to RE2 as it is. This scanner reads the pattern as ECMA-262 does
// in Unicode mode and writes the RE2 expression with the same meaning, in which every literal character that is not
// a letter, a digit or '_' is a hexadecimal escape.
//
// What it translates exactly: literal characters, '.', the anchors '^' and '$', "\b" and "\B", groups "(...)",
// "(?:...)" and "(?<name>...)", alternation, the quantifiers '*', '+', '?', "{n}", "{n,}" and "{n,m}" with their lazy
// forms, character classes with ranges, the class escapes "\d \D \w \W \s \S", the control, hexadecimal and Unicode
// escapes ("\t \n \v \f \r \cX \0 \xHH \uHHHH \u{H...}" and surrogate pairs of "\u" escapes), escaped syntax
// characters, and "\p{..}" and "\P{..}" for the general categories that RE2 defines as ECMA-262 does.
//
// What it refuses (RE2 has no such construct, or reads it differently): lookahead and lookbehind assertions,
// back-references (numbered and named), group modifiers, Unicode properties other than those general categories,
// repeat counts above 1000, and everything that is a syntax error in Unicode mode: an identity escape of a character
// that is not a syntax character, an unescaped ']', '{' or '}', a quantifier with nothing to repeat, a class range
// that is reversed or that has a class escape at an end, a repeated group name, a lone surrogate escape, text that
// is not valid UTF-8.
//
// The position of a refusal is the byte offset at which the construct begins.
std::variant<std::string, PatternRefusal> translate_pattern(std::string_view pattern);

// Whether `text` is well-formed UTF-8: no overlong form, no surrogate, nothing above U+10FFFF.
bool is_valid_utf8(std::string_view text);

}  // namespace neomcp::json_schema::detail
