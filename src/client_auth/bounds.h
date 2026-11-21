// The bounds of the OAuth flow on what a server or an authorization server controls, each with its default.
//
// Every value a peer can make large or slow has a bound here. The application sets most of them (OAuthBounds of
// neomcp/client/oauth_types.h); the flow's own bounds add the two the transport owns (how often one request may be
// refused and held, how many redirects a metadata request follows), which the driver passes, and the fixed number of
// refresh attempts.
#pragma once

#include <cstddef>

#include "neomcp/client/oauth_types.h"

namespace neomcp::detail::oauth {

struct OAuthDefaults : neomcp::OAuthDefaults {
    // Redirects of a metadata request (registration and token requests follow none): the same number as the HTTP
    // transport's default for its own requests (HttpClientDefaults::max_redirects).
    static constexpr std::size_t max_redirects = 5;
    // How many times one request may be refused and held for authorization.
    static constexpr std::size_t max_authorizations_per_request = 3;
    // Refresh attempts in one flow; not an option.
    static constexpr std::size_t refresh_attempts_per_flow = 1;
};

struct OAuthBounds : neomcp::OAuthBounds {
    std::size_t max_redirects = OAuthDefaults::max_redirects;
    std::size_t max_authorizations_per_request = OAuthDefaults::max_authorizations_per_request;
};

}  // namespace neomcp::detail::oauth
