// The token requests (OAuth 2.1, sections 4.1.3 and 4.3) and the check of their answers.
//
// Both are form-encoded POSTs to the token endpoint that follow no redirect, with the `resource` of the protected
// resource metadata and the client's authentication: by the Basic scheme with the form-encoded identifier and secret,
// with both in the body, or with the identifier alone in the body. No other request carries the secret, the code or
// the verifier.
//
// An answer is used when it is a 200 with a JSON object holding an `access_token` of the b64token syntax (it is sent
// in a header field) within its bound and a `token_type` of Bearer, compared without regard to case. Any other token
// type is refused.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "client_auth/bounds.h"
#include "client_auth/registration.h"
#include "client_auth/types.h"

namespace neomcp::detail::oauth {

// The b64token syntax of RFC 6750, section 2.1: 1*( ALPHA / DIGIT / "-" / "." / "_" / "~" / "+" / "/" ) *"=".
bool is_b64token(std::string_view text) noexcept;

FlowFetch authorization_code_request(std::string_view token_endpoint, const ClientCredentials& client,
                                     std::string_view code, std::string_view redirect_uri,
                                     std::string_view code_verifier, const std::optional<std::string>& resource,
                                     const OAuthBounds& bounds);

FlowFetch refresh_token_request(std::string_view token_endpoint, const ClientCredentials& client,
                                std::string_view refresh_token, const std::optional<std::string>& resource,
                                const OAuthBounds& bounds);

// The tokens of a token answer, or why it cannot be used. `requested` are the scopes the authorization asked for,
// taken as granted when the answer names none, and kept as the tokens' requested scopes.
std::variant<StoredTokens, FlowFailure> read_token_answer(const FetchResult& result, std::string_view issuer,
                                                          std::string_view token_endpoint,
                                                          const std::vector<std::string>& requested,
                                                          FlowClock::time_point now, const OAuthBounds& bounds,
                                                          std::vector<FlowDiagnostic>& diagnostics);

}  // namespace neomcp::detail::oauth
