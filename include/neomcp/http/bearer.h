// The server's side of the Bearer scheme of OAuth (RFC 6750): reading the credentials of an Authorization field, and
// writing the Bearer challenge of a WWW-Authenticate field, with the resource_metadata parameter of RFC 9728 (section
// 5.1). The reader of the challenges a client gets is in challenge.h; what is written here reads back through it.
//
// The credentials are read by a parser of the two productions, never by pattern:
//
//   credentials = "Bearer" 1*SP b64token
//   b64token    = 1*( ALPHA / DIGIT / "-" / "." / "_" / "~" / "+" / "/" ) *"="
//
// The scheme name is compared without regard to ASCII case (RFC 9110, section 11.1). The field value is read without
// the whitespace (SP, HTAB) before and after it, as a field value is (RFC 9110, section 5.5). Only the Authorization
// field is read: the form-encoded body and the query string of RFC 6750 (sections 2.2 and 2.3) are never looked at.
// Nothing throws for what a peer sent, and no result carries the peer's text apart from the token itself.
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "neomcp/http/headers.h"

namespace neomcp::http {

// The names of the two header fields.
inline constexpr std::string_view authorization_field_name = "Authorization";
inline constexpr std::string_view www_authenticate_field_name = "WWW-Authenticate";

// ---- Reading the credentials

struct CredentialsLimits {
    // The token a client may present, in bytes. The default is the bound the client side of this library keeps its
    // tokens within, so the two sides agree.
    static constexpr std::size_t default_max_token_bytes = std::size_t{16} * 1024;

    std::size_t max_token_bytes = default_max_token_bytes;
};

// A Bearer token as presented: exactly the b64token, with no whitespace around it.
struct BearerCredentials {
    std::string token;
};

// Why no token was read. A server maps these to its answers (RFC 6750, section 3.1):
//   absent, other_scheme   no credentials of the Bearer scheme: a challenge without an error code
//   repeated, malformed    a request that cannot be read: 400, error "invalid_request"
//   token_too_large        a well-formed token over the bound: 401, error "invalid_token", and no validator is asked
enum class CredentialsProblem {
    absent,          // no Authorization field
    other_scheme,    // one field, whose scheme is not Bearer (the rest of the value is not read)
    repeated,        // more than one Authorization field: the token may not be taken from either
    malformed,       // a Bearer scheme not followed by 1*SP b64token, an empty value, or a value without a scheme
    token_too_large  // a b64token longer than CredentialsLimits::max_token_bytes
};

// The kind of problem, for a person to read; never the peer's text.
std::string_view to_string(CredentialsProblem problem) noexcept;

// The token of the Authorization fields given (the values of every field of that name, in order), or why there is none.
std::variant<BearerCredentials, CredentialsProblem> read_bearer_credentials(std::span<const std::string_view> fields,
                                                                            const CredentialsLimits& limits = {});

// The same for the Authorization fields of a header collection.
std::variant<BearerCredentials, CredentialsProblem> read_bearer_credentials(const Headers& headers,
                                                                            const CredentialsLimits& limits = {});

// ---- Scope values

// A scope value: scope-token = 1*( %x21 / %x23-5B / %x5D-7E ) (RFC 6749, appendix A.4). The text is opaque: no
// meaning is read from it. A space, a quote, a backslash, a control byte and a byte over 0x7E are not allowed.
bool is_scope_token(std::string_view value) noexcept;

// ---- Writing a Bearer challenge

struct BearerChallengeParts {
    // The URL of the protected resource metadata, written verbatim. Not empty; the characters of a URI reference
    // (%x21 / %x23-5B / %x5D-7E).
    std::optional<std::string> resource_metadata;
    // Scope values, written joined by one space as the `scope` parameter; the parameter is left out when there are
    // none. Each is a scope token.
    std::vector<std::string> scopes;
    // An error code. Not empty; characters %x20-21 / %x23-5B / %x5D-7E (RFC 6750, section 3).
    std::optional<std::string> error;
    // A text for developers. Not empty; characters %x20-21 / %x23-5B / %x5D-7E (RFC 6750, section 3), so no quote and
    // no backslash. The library writes none itself: a text taken from a validator's reason is not under its control.
    std::optional<std::string> error_description;
};

enum class ChallengeWriteProblem {
    invalid_resource_metadata,
    invalid_scope,
    invalid_error,
    invalid_error_description
};

// The kind of problem, for a person to read; never the value.
std::string_view to_string(ChallengeWriteProblem problem) noexcept;

// The value of a WWW-Authenticate field: `Bearer`, then the parameters present as quoted strings separated by ", ", in
// the order error, error_description, scope, resource_metadata; a parameter never occurs twice. A value that cannot be
// written (empty where a value is required, or with a character the parameter does not allow) refuses the whole
// challenge: nothing is written raw. No parameter allows the quote or the backslash, so a written value is never
// escaped; the quoting still escapes them, so that none could end the string.
std::variant<std::string, ChallengeWriteProblem> write_bearer_challenge(const BearerChallengeParts& parts);

}  // namespace neomcp::http
