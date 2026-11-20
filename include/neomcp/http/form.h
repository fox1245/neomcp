// The application/x-www-form-urlencoded format as OAuth 2.1 uses it (draft-ietf-oauth-v2-1, appendix B): the
// parameters of an authorization request and of its response, and the body of a token request; and the credentials
// of the Basic scheme with which an OAuth client authenticates at a token endpoint (section 2.4.1 of the same draft).
//
// Encoding: names and values are taken as UTF-8 octets. A space becomes '+'; ASCII letters, digits and the other
// unreserved characters of RFC 3986 ("-", ".", "_", "~") stay as they are; every other octet becomes '%' and two
// upper-case hexadecimal digits. So the six code points SPACE, PERCENT SIGN, AMPERSAND, PLUS SIGN, POUND SIGN and EURO
// SIGN encode as "+%25%26%2B%C2%A3%E2%82%AC", the draft's own example.
//
// Decoding reverses it in one pass: '+' is a space and "%HH" the octet HH (either case of hex digit), so "%2B" is a
// plus sign. The decoded octets must be UTF-8. Nothing here throws for what a peer sent, and no error carries the
// peer's text: a problem is a kind and a place. Nothing depends on an HTTP library.
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace neomcp::http {

struct FormField {
    std::string name;
    std::string value;
    friend bool operator==(const FormField&, const FormField&) = default;
};

// One name or value, encoded.
std::string form_encode_component(std::string_view text);

// The fields in order as `name=value` joined by '&'. No field gives an empty text.
std::string form_encode(std::span<const FormField> fields);

enum class FormProblem {
    bad_escape,   // a '%' not followed by two hexadecimal digits
    invalid_utf8  // a name or value whose decoded octets are not UTF-8
};

// The kind of problem, for a person to read; never the peer's text.
std::string_view to_string(FormProblem problem) noexcept;

struct FormError {
    FormProblem problem = FormProblem::bad_escape;
    std::size_t offset = 0;  // the byte offset in the text: of the '%', or of the name or value that is not UTF-8
};

// The fields of a text in order. The text is split at '&'; an empty piece is skipped; a piece is split at its first
// '=', and a piece without one is a name with an empty value. Fields that repeat a name are all kept, so a caller can
// refuse them; find_unique_field says how a name occurs. The text is the part after the '?' of a URL, or a body.
std::variant<std::vector<FormField>, FormError> form_decode(std::string_view text);

// How a name occurs among decoded fields, read as OAuth 2.1 reads request and response parameters (sections 3.1 and
// 3.2): a field whose value is empty counts as absent, and a name with a value in more than one field is repeated.
// Names compare exactly.
struct FieldLookup {
    enum class State { absent, present, repeated };
    State state = State::absent;
    std::string_view value;  // the value when present; a view into the fields
};
FieldLookup find_unique_field(std::span<const FormField> fields, std::string_view name);

// The value of an Authorization field with which an OAuth client authenticates by the Basic scheme: "Basic " and the
// Base64 (RFC 4648, section 4, padded) of the encoded client identifier, ':' and the encoded client secret. The
// encoding keeps a ':' of the identifier from being read as the separator.
std::string oauth_basic_authorization(std::string_view client_id, std::string_view client_secret);

}  // namespace neomcp::http
