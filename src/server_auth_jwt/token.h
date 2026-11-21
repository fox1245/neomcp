// The reading of a JWT access token's parts and of the authorization server's metadata, without I/O and without
// signatures. Internal to neomcp::server_auth_jwt; include/neomcp/http/jwt.h says what is accepted.
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "neomcp/http/jwt.h"
#include "neomcp/http/resource_server.h"
#include "neomcp/json.h"

namespace neomcp::http::detail {

// The deepest nesting of a header, a claims set, a key set or a metadata document that is read. A key set is an object
// that holds an array of objects that hold arrays of strings; claims may hold some structure of their own.
inline constexpr std::size_t max_document_depth = 16;

// A JSON object read from `text`, or nothing when the text is not one JSON object within max_document_depth, or when a
// name occurs twice in any object of it (RFC 7519, section 4; RFC 7515, section 4).
std::optional<json> parse_unique_object(std::string_view text);

// The member `name` of an object, or null.
const json* member(const json& object, std::string_view name);
// The member `name` of an object when it is a string, or null.
const std::string* string_member(const json& object, std::string_view name);

// The three parts of a token in the JWS compact form (RFC 7515, section 7.1), decoded, and the text the signature is
// over (the first two parts as received, with the '.' between them). Views into the token given to split_compact.
struct CompactParts {
    std::string_view signing_input;
    std::string header;
    std::string payload;
    std::string signature;
};

// Nothing when the token is not three parts of strict base64url without padding.
std::optional<CompactParts> split_compact(std::string_view token);

// What a token's header says, as far as the validator uses it.
struct JoseHeader {
    JwtAlgorithm algorithm = JwtAlgorithm::rs256;
    std::optional<std::string> kid;
};

// The header, or the event that refuses the token (malformed, algorithm_refused, type_refused, critical_header).
std::variant<JoseHeader, JwtEvent> read_header(std::string_view header_json, std::span<const JwtAlgorithm> allowed);

// The algorithm a JOSE name stands for, among those given; nothing when it is none of them.
std::optional<JwtAlgorithm> algorithm_named(std::string_view name, std::span<const JwtAlgorithm> allowed);

// What the claims of a token whose signature is good say: an Authorization, or the event that refuses the token
// (malformed, issuer_mismatch).
std::variant<Authorization, JwtEvent> read_claims(std::string_view payload_json, std::string_view issuer);

// The `jwks_uri` of an authorization server's metadata document (RFC 8414, section 2), when the document is one JSON
// object whose `issuer` is `issuer` exactly and whose `jwks_uri` is a string.
std::optional<std::string> read_metadata_jwks_uri(std::string_view body, std::string_view issuer);

}  // namespace neomcp::http::detail
