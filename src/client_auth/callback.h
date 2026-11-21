// The authorization request (OAuth 2.1, section 4.1.1, with PKCE and the resource indicator of RFC 8707) and the
// check of the response the person's browser brings back to the redirect URI.
//
// The callback is checked in this order, and the first failure ends the flow:
//   1. it is within its bound, reads as a URL, and has the redirect URI's scheme, host, port and path; its query is
//      form-decoded and no parameter occurs twice with a value (a parameter without value counts as absent);
//   2. `iss` (RFC 9207, section 2.4): when present, it must be the recorded issuer, compared character by character
//      after form decoding and nothing else; when absent, the response is refused if the server advertises that it
//      sends `iss`. A refusal here uses nothing of the response, its error included;
//   3. `state` must be the one this flow sent, exactly;
//   4. an `error` ends the flow with its code (never its description or URI);
//   5. `code` must be present.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "client_auth/bounds.h"
#include "client_auth/types.h"

namespace neomcp::detail::oauth {

struct AuthorizationRequest {
    std::string authorization_endpoint;
    std::string client_id;
    std::string redirect_uri;
    std::string code_challenge;
    std::string state;
    std::optional<std::string> resource;
    std::vector<std::string> scopes;  // no `scope` parameter when empty
};

// The authorization endpoint with the request's parameters added to its query (after any query it already has).
std::string authorization_url(const AuthorizationRequest& request);

// What a flow records before the person is sent to the authorization server.
struct PendingAuthorization {
    std::string issuer;  // from the validated metadata
    bool iss_parameter_supported = false;
    std::string state;
    std::string redirect_uri;
};

// The authorization code of the callback, or why the response is refused.
std::variant<std::string, FlowFailure> check_callback(std::string_view callback_url,
                                                      const PendingAuthorization& pending,
                                                      const OAuthBounds& bounds);

}  // namespace neomcp::detail::oauth
