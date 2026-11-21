// Authorization server metadata (RFC 8414, and OpenID Connect Discovery as RFC 8414, section 5 relates them).
//
// Candidates, from the issuer identifier as received: with a path p (a terminating '/' removed),
// `/.well-known/oauth-authorization-server` + p, `/.well-known/openid-configuration` + p, then
// p + `/.well-known/openid-configuration`; without a path the first two without p. They are tried in that order; a
// 404 or another 4xx except 429 goes on to the next one, anything else that is not a document ends the flow.
//
// A document is used only when its `issuer` is identical to the identifier it was found by, character by character
// (RFC 8414, section 3.3). Another issuer ends the flow at once: no later candidate is tried and nothing of that
// document is used. A document must name the authorization and token endpoints, and must list S256 among
// `code_challenge_methods_supported`: S256 is the only method this client sends, and a server that lists none is
// refused as well.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "client_auth/admission.h"
#include "client_auth/bounds.h"
#include "client_auth/types.h"
#include "neomcp/json.h"

namespace neomcp::detail::oauth {

inline constexpr std::string_view oauth_metadata_well_known_suffix = "oauth-authorization-server";
inline constexpr std::string_view openid_configuration_well_known_suffix = "openid-configuration";

// The metadata URLs for an issuer identifier, in order, or why there are none (an identifier that is not an absolute
// https or loopback http URL without query and fragment, or a URL the policy refuses).
std::variant<std::vector<std::string>, FlowFailure> authorization_server_candidates(std::string_view issuer,
                                                                                    const UrlAdmission& admission);

struct AuthorizationServer {
    std::string issuer;  // as received, identical to the identifier
    std::string authorization_endpoint;
    std::string token_endpoint;
    std::optional<std::string> registration_endpoint;
    std::vector<std::string> scopes_supported;
    // Absent: the server lists none, which means client_secret_basic (RFC 8414, section 2).
    std::optional<std::vector<std::string>> token_endpoint_auth_methods_supported;
    bool client_id_metadata_document_supported = false;
    bool authorization_response_iss_parameter_supported = false;
};

// The metadata of a document found by `issuer`, or why it cannot be used.
std::variant<AuthorizationServer, FlowFailure> read_authorization_server(const json& document,
                                                                         std::string_view issuer,
                                                                         std::string_view metadata_url,
                                                                         const UrlAdmission& admission,
                                                                         const OAuthBounds& bounds,
                                                                         std::vector<FlowDiagnostic>& diagnostics);

}  // namespace neomcp::detail::oauth
