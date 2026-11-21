// Absolute URLs as the OAuth flow reads them: split into their parts without changing a character, so that an
// identifier is compared and echoed exactly as it was received and never written back in another form.
//
// The reader is strict: a scheme, "://", an authority of a host and an optional port (no user information), then a
// path, a query and a fragment made only of the characters RFC 3986 allows there, with every '%' followed by two
// hexadecimal digits. Whatever it accepts reads the same to every RFC 3986 parser, so a URL built here from the parts
// of another reads at the fetch as it reads here. A URL to be fetched is still admitted by the URL policy of the
// transport, which reads it with the parser of the request.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "neomcp/http/authority.h"

namespace neomcp::detail::oauth {

inline constexpr std::string_view scheme_http = "http";
inline constexpr std::string_view scheme_https = "https";

// The parts of an absolute URL, as views into its text.
struct UrlParts {
    std::string_view text;       // the whole URL
    std::string_view scheme;     // as written
    std::string_view authority;  // as written: host and optional ":port"
    std::string_view host;       // as written (an IPv6 address with its brackets)
    std::string_view port;       // the digits as written; empty when there is no port
    std::string_view path;       // as written; may be empty
    std::optional<std::string_view> query;     // after '?', without it
    std::optional<std::string_view> fragment;  // after '#', without it
    neomcp::http::Authority parsed;      // the host in lower case and the port as a number, for origin comparisons
};

// The parts of `text`, or nothing when it is not an absolute URL of the strict form above. The parts are views into
// `text`, so a temporary string is refused at compile time.
std::optional<UrlParts> read_url(std::string_view text);
std::optional<UrlParts> read_url(std::string&& text) = delete;
inline std::optional<UrlParts> read_url(const char* text) { return read_url(std::string_view(text)); }

// `scheme` is `name`, ignoring ASCII case.
bool scheme_is(const UrlParts& url, std::string_view name) noexcept;

// The same scheme, host and port: schemes and hosts without regard to case, a missing port taken as the scheme's
// default (80 for http, 443 for https).
bool same_origin(const UrlParts& a, const UrlParts& b) noexcept;

// The host is loopback: the name `localhost`, an IPv4 address in 127.0.0.0/8, or the IPv6 address ::1. Decided from
// the parsed host, never by searching the text.
bool is_loopback(const UrlParts& url) noexcept;

// The URL may carry credentials: https, or http to a loopback host.
bool is_secure_or_loopback(const UrlParts& url) noexcept;

// The text without its fragment.
std::string_view without_fragment(const UrlParts& url) noexcept;

// The text without its query and fragment: what a failure may name.
std::string without_query(std::string_view text);

// `scheme "://" authority` as written, without a trailing slash.
std::string origin_text(const UrlParts& url);

// The URL of a well-known document for the identifier `url`, built by inserting `/.well-known/<suffix>` between the
// authority and the path (RFC 8615, as RFC 8414 and RFC 9728 use it). A terminating '/' of the path is removed first;
// the query is kept when `keep_query` is set. The fragment is never kept.
std::string insert_well_known(const UrlParts& url, std::string_view suffix, bool keep_query);

// The URL of a well-known document appended to the path of `url` (OpenID Connect Discovery's form): the path without
// a terminating '/', then `/.well-known/<suffix>`. Query and fragment are not kept.
std::string append_well_known(const UrlParts& url, std::string_view suffix);

// Whether two resource identifiers are identical in the sense of RFC 9728, section 3.3, with the scheme and host
// compared without regard to case (the MCP authorization page asks clients to accept upper-case scheme and host). The
// port, path, query and fragment compare character by character: a written default port, a trailing slash, or a
// percent-encoding of a character that needs none is a difference. Both must read as URLs.
bool same_resource_identifier(std::string_view expected, std::string_view received);

}  // namespace neomcp::detail::oauth
