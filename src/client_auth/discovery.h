// Protected resource metadata (RFC 9728): where the flow looks for it, and what it takes from it.
//
// Where: when the server's Bearer challenge names `resource_metadata`, that URL only, which must have the endpoint's
// origin; otherwise the URL built by inserting the well-known path between the endpoint's authority and its path
// (query kept), then the one at the root of the endpoint's origin. When the endpoint has no path both are one URL,
// requested once.
//
// What: `resource` must be identical to the identifier the URL was built from (RFC 9728, section 3.3): the endpoint's
// URL for the URL the server named and for the path form, the endpoint's origin as written for the root form. Scheme
// and host compare without regard to case, everything else character by character. Another value is refused before
// any authorization server is asked, unless the application's hook accepts it. The first entry of
// `authorization_servers` that is admitted becomes the authorization server; the identifiers are kept as received.
#pragma once

#include <functional>
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

inline constexpr std::string_view protected_resource_well_known_suffix = "oauth-protected-resource";

struct PrmCandidate {
    std::string url;
    // The identifiers `resource` may be identical to: the one the URL was built from (two when the path form and the
    // root form are one URL).
    std::vector<std::string> identifiers;
    bool named_by_server = false;
};

// The candidate URLs in the order they are tried, or why none can be.
std::variant<std::vector<PrmCandidate>, FlowFailure> protected_resource_candidates(
    std::string_view endpoint_url, const std::optional<std::string>& resource_metadata, const UrlAdmission& admission);

// The resource identifier the flow expected and the one the metadata holds, for the application's hook.
struct ResourceMismatch {
    std::string expected;
    std::string received;
};
// True: use the metadata although its resource differs. Not set: refuse it.
using AcceptResource = std::function<bool(const ResourceMismatch&)>;

struct ProtectedResource {
    std::string resource;              // as received: sent as the `resource` parameter
    std::string authorization_server;  // as received: the issuer identifier
    std::vector<std::string> scopes_supported;
    std::string metadata_url;
};

std::variant<ProtectedResource, FlowFailure> read_protected_resource(const json& document,
                                                                     const PrmCandidate& candidate,
                                                                     const AcceptResource& accept_resource,
                                                                     const UrlAdmission& admission,
                                                                     const OAuthBounds& bounds,
                                                                     std::vector<FlowDiagnostic>& diagnostics);

}  // namespace neomcp::detail::oauth
