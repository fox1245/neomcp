// Absolute URLs as the OAuth flow reads them (url.h).
#include "client_auth/url.h"

#include <cstdint>
#include <utility>

#include "http_base/ip_text.h"

namespace neomcp::detail::oauth {

namespace {

constexpr std::string_view scheme_separator = "://";
constexpr std::string_view well_known_prefix = "/.well-known/";
constexpr std::string_view loopback_name = "localhost";
constexpr std::string_view ipv6_loopback = "::1";
constexpr std::string_view ipv4_loopback_first_octet = "127";
constexpr std::uint16_t default_port_http = 80;
constexpr std::uint16_t default_port_https = 443;

bool is_alpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }
bool is_hex(char c) noexcept { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool equal_ignoring_case(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

bool is_unreserved(char c) noexcept {
    return is_alpha(c) || is_digit(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

bool is_sub_delim(char c) noexcept {
    switch (c) {
        case '!': case '$': case '&': case '\'': case '(': case ')': case '*': case '+': case ',': case ';': case '=':
            return true;
        default:
            return false;
    }
}

// RFC 3986: pchar = unreserved / pct-encoded / sub-delims / ":" / "@"; the path adds '/', query and fragment add '/'
// and '?'. A '%' must start two hexadecimal digits.
bool valid_component(std::string_view text, bool allow_question_mark) noexcept {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '%') {
            if (text.size() - i < 3) return false;
            if (!is_hex(text[i + 1]) || !is_hex(text[i + 2])) return false;
            i += 2;
            continue;
        }
        if (is_unreserved(c) || is_sub_delim(c) || c == ':' || c == '@' || c == '/') continue;
        if (allow_question_mark && c == '?') continue;
        return false;
    }
    return true;
}

std::uint16_t effective_port(const UrlParts& url) noexcept {
    if (url.parsed.port) return *url.parsed.port;
    if (scheme_is(url, scheme_https)) return default_port_https;
    if (scheme_is(url, scheme_http)) return default_port_http;
    return 0;
}

std::string_view path_without_terminating_slash(std::string_view path) noexcept {
    if (!path.empty() && path.back() == '/') path.remove_suffix(1);
    return path;
}

}  // namespace

std::optional<UrlParts> read_url(std::string_view text) {
    UrlParts url;
    url.text = text;
    if (text.empty() || !is_alpha(text.front())) return std::nullopt;
    std::size_t at = 1;
    while (at < text.size() && (is_alpha(text[at]) || is_digit(text[at]) || text[at] == '+' || text[at] == '-' ||
                                text[at] == '.')) {
        ++at;
    }
    if (text.substr(at, scheme_separator.size()) != scheme_separator) return std::nullopt;
    url.scheme = text.substr(0, at);
    std::string_view rest = text.substr(at + scheme_separator.size());

    const std::size_t authority_end = rest.find_first_of("/?#");
    url.authority = rest.substr(0, authority_end);
    rest = authority_end == std::string_view::npos ? std::string_view() : rest.substr(authority_end);
    auto parsed = neomcp::http::parse_authority(url.authority);
    if (!parsed) return std::nullopt;
    url.parsed = std::move(*parsed);
    if (url.authority.front() == '[') {
        const std::size_t close = url.authority.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        url.host = url.authority.substr(0, close + 1);
        if (close + 1 < url.authority.size()) url.port = url.authority.substr(close + 2);
    } else {
        const std::size_t colon = url.authority.rfind(':');
        url.host = url.authority.substr(0, colon);
        if (colon != std::string_view::npos) url.port = url.authority.substr(colon + 1);
    }

    if (const std::size_t hash = rest.find('#'); hash != std::string_view::npos) {
        url.fragment = rest.substr(hash + 1);
        rest = rest.substr(0, hash);
        if (!valid_component(*url.fragment, true)) return std::nullopt;
    }
    if (const std::size_t mark = rest.find('?'); mark != std::string_view::npos) {
        url.query = rest.substr(mark + 1);
        rest = rest.substr(0, mark);
        if (!valid_component(*url.query, true)) return std::nullopt;
    }
    url.path = rest;
    if (!valid_component(url.path, false)) return std::nullopt;
    return url;
}

bool scheme_is(const UrlParts& url, std::string_view name) noexcept { return equal_ignoring_case(url.scheme, name); }

bool same_origin(const UrlParts& a, const UrlParts& b) noexcept {
    return equal_ignoring_case(a.scheme, b.scheme) && a.parsed.host == b.parsed.host &&
           a.parsed.ipv6 == b.parsed.ipv6 && effective_port(a) == effective_port(b) && effective_port(a) != 0;
}

bool is_loopback(const UrlParts& url) noexcept {
    const std::string& host = url.parsed.host;
    if (url.parsed.ipv6) return host == ipv6_loopback;
    if (host == loopback_name) return true;
    if (!neomcp::http::detail::is_ipv4_text(host)) return false;
    return std::string_view(host).substr(0, host.find('.')) == ipv4_loopback_first_octet;
}

bool is_secure_or_loopback(const UrlParts& url) noexcept {
    if (scheme_is(url, scheme_https)) return true;
    return scheme_is(url, scheme_http) && is_loopback(url);
}

std::string_view without_fragment(const UrlParts& url) noexcept {
    if (!url.fragment) return url.text;
    // The fragment and its '#' are the end of the text.
    return url.text.substr(0, url.text.size() - url.fragment->size() - 1);
}

std::string without_query(std::string_view text) {
    return std::string(text.substr(0, text.find_first_of("?#")));
}

std::string origin_text(const UrlParts& url) {
    std::string out(url.scheme);
    out.append(scheme_separator);
    out.append(url.authority);
    return out;
}

std::string insert_well_known(const UrlParts& url, std::string_view suffix, bool keep_query) {
    std::string out = origin_text(url);
    out.append(well_known_prefix);
    out.append(suffix);
    out.append(path_without_terminating_slash(url.path));
    if (keep_query && url.query) {
        out.push_back('?');
        out.append(*url.query);
    }
    return out;
}

std::string append_well_known(const UrlParts& url, std::string_view suffix) {
    std::string out = origin_text(url);
    out.append(path_without_terminating_slash(url.path));
    out.append(well_known_prefix);
    out.append(suffix);
    return out;
}

bool same_resource_identifier(std::string_view expected, std::string_view received) {
    const auto a = read_url(expected);
    const auto b = read_url(received);
    if (!a || !b) return false;
    return equal_ignoring_case(a->scheme, b->scheme) && equal_ignoring_case(a->host, b->host) && a->port == b->port &&
           a->path == b->path && a->query == b->query && a->fragment == b->fragment;
}

}  // namespace neomcp::detail::oauth
