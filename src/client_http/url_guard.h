// The one check every URL the client reaches on a server's word passes: the target of a redirect, the place the older
// event-stream transport names for messages, and (with authorization) the documents a server points to.
//
// A UrlGuard is made for the endpoint the application named. For a redirect and for the place messages go it admits a
// URL only when the URL uses http or https, carries no user information, and has the endpoint's origin: the same
// scheme, host and port, compared after the scheme and host are folded to lower case and a missing port is taken as
// the scheme's default. URLs are read with libcurl's own URL parser, the one that later reads the URL a request goes
// to, so the check and the request cannot read one text two ways.
//
// A document for authorization (UrlPurpose::authorization) may be on another origin, since an authorization server
// usually is, under a stricter rule: an absolute URL without user information or fragment; https, or http only to a
// loopback host; and a loopback host, whatever the scheme, only when the endpoint's own host is loopback, so that a
// server elsewhere cannot point the client at services of the machine it runs on. A host is loopback when the parser
// reads it as an IPv4 address in 127.0.0.0/8, the IPv6 address ::1 (or an IPv4 loopback address mapped into IPv6), or
// the name `localhost` (one trailing dot allowed). Hosts that reach this machine without being loopback names in that
// sense (the unspecified addresses 0.0.0.0/8 and ::, names under `.localhost`) are never admitted for authorization.
//
// A UrlGuard holds only values; it may be used from any thread.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace neomcp::detail::http_client {

// Why a URL is to be reached; part of what a refusal says.
enum class UrlPurpose {
    redirect,        // the Location of a redirect
    event_endpoint,  // the older transport: where messages go
    authorization    // a document the server points to for authorization
};

struct Origin {
    std::string scheme;  // lower case: http or https
    std::string host;    // lower case, as the URL wrote it (an IPv6 address in brackets)
    std::uint16_t port = 0;  // the scheme's default when the URL names none
    friend bool operator==(const Origin&, const Origin&) = default;
};

// What a host is to this machine.
enum class HostReach {
    loopback,  // a loopback address or the name localhost
    local,     // reaches this machine without being a loopback name (an unspecified address, a name under .localhost)
    other      // anything else
};

// What `host` (as Origin::host holds it: lower case, an IPv6 address in brackets) is to this machine, read with an
// address parser; never from a part of the text.
HostReach reach_of(std::string_view host);

// The origin of an absolute URL with scheme http or https and without user information; why not, otherwise.
std::variant<Origin, std::string> origin_of(std::string_view url);

// `reference` (absolute, or relative to `base`) resolved against the absolute `base`, as libcurl resolves a Location;
// empty when either cannot be read.
std::optional<std::string> resolve_reference(std::string_view base, std::string_view reference);

class UrlGuard {
public:
    // A guard for the endpoint at `endpoint_url`; why the URL cannot serve as an endpoint, otherwise.
    static std::variant<UrlGuard, std::string> for_endpoint(std::string_view endpoint_url);

    // Empty when `url` may be reached for `purpose`; why not, otherwise.
    std::optional<std::string> admit(UrlPurpose purpose, std::string_view url) const;

    const Origin& origin() const noexcept { return origin_; }

    // Whether a token may go to the endpoint: it is https, or its host is loopback.
    bool carries_tokens() const noexcept;

private:
    explicit UrlGuard(Origin origin);
    Origin origin_;
    HostReach reach_ = HostReach::other;
};

}  // namespace neomcp::detail::http_client
