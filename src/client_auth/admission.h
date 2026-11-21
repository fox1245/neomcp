// The check every URL a server or an authorization server supplies passes before the flow uses it.
//
// The flow does not decide which hosts may be reached; the transport's URL policy does, reading each URL with the
// parser of the request that will fetch it. The flow asks it about every URL it would fetch or hand to the person:
// the protected resource metadata URL, the authorization server identifier and the URLs built from it, and the
// endpoints the metadata names.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace neomcp::detail::oauth {

enum class UrlRole {
    protected_resource_metadata,     // a protected resource metadata URL, named by the server or built
    authorization_server,            // an authorization server identifier
    authorization_server_metadata,   // a metadata URL built from an identifier
    authorization_endpoint,
    token_endpoint,
    registration_endpoint
};

class UrlAdmission {
public:
    virtual ~UrlAdmission() = default;
    // Empty when `url` may be used in `role`; why not, otherwise (the policy's own words, never the URL's query).
    virtual std::optional<std::string> admit(UrlRole role, std::string_view url) const = 0;
};

}  // namespace neomcp::detail::oauth
