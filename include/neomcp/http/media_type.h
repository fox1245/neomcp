// Media types as HTTP writes them: the value of a Content-Type field and the media ranges of an Accept field.
//
// Both are read by a parser of the grammar of RFC 9110 (sections 8.3.1, 5.6 and 12.5.1), never by pattern, and a
// value the grammar does not allow is invalid as a whole: nothing is guessed from the part that could be read.
#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neomcp::http {

struct MediaType {
    std::string type;     // lower case
    std::string subtype;  // lower case
    // In order; names lower case, values as written (a quoted string without its quotes and escapes).
    std::vector<std::pair<std::string, std::string>> parameters;
};

// Reads `type "/" subtype *( OWS ";" OWS [ name "=" ( token / quoted-string ) ] )`. Returns nothing for an empty
// value, a missing or empty subtype, a character outside the grammar, or a parameter without a value.
std::optional<MediaType> parse_media_type(std::string_view text);

// True when `text` is a media type of that type and subtype, compared without regard to case; parameters (a charset)
// do not matter. False for an invalid value.
bool is_media_type(std::string_view text, std::string_view type, std::string_view subtype);

struct MediaRange {
    std::string type;     // lower case; "*" for any type
    std::string subtype;  // lower case; "*" for any subtype
    int weight = 1000;    // the q value times 1000: 0 to 1000
};

// Reads an Accept value: a comma-separated list of media ranges ("*/*", "type/*" or "type/subtype"), each with
// optional parameters, among them the weight `q`. Empty list elements are allowed, so "" is an empty list. Returns
// nothing for a value the grammar does not allow, a range like "*/json", or a weight that is not a qvalue (0 to 1
// with at most three decimals).
std::optional<std::vector<MediaRange>> parse_accept(std::string_view text);

// Whether the ranges admit a response of that media type: the most specific range that matches it ("type/subtype"
// over "type/*" over "*/*") has a weight above zero; among equally specific ones the highest weight counts. Parameters
// of a range other than its weight do not narrow it. An empty list admits nothing.
bool accepts(std::span<const MediaRange> ranges, std::string_view type, std::string_view subtype);

}  // namespace neomcp::http
