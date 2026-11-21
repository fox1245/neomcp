// The guard of URLs a server supplies (url_guard.h).
#include "client_http/url_guard.h"

#include <charconv>
#include <memory>
#include <system_error>

#include <asio/ip/address_v4.hpp>
#include <asio/ip/address_v6.hpp>
#include <curl/curl.h>

namespace neomcp::detail::http_client {

namespace {

constexpr std::string_view scheme_http = "http";
constexpr std::string_view scheme_https = "https";
// The reserved name of this machine (RFC 6761, section 6.3), and the domain under it.
constexpr std::string_view loopback_name = "localhost";
constexpr char label_separator = '.';
// IPv4: 127.0.0.0/8 is loopback; 0.0.0.0/8 names "this host on this network", which reaches this machine.
constexpr unsigned char ipv4_loopback_octet = 127;
constexpr unsigned char ipv4_this_network_octet = 0;
constexpr char ipv6_open = '[';
constexpr char ipv6_close = ']';

HostReach reach_of_v4(const asio::ip::address_v4& address) {
    const auto octets = address.to_bytes();
    if (octets[0] == ipv4_loopback_octet) return HostReach::loopback;
    if (octets[0] == ipv4_this_network_octet) return HostReach::local;
    return HostReach::other;
}

struct UrlFree {
    void operator()(CURLU* url) const noexcept { curl_url_cleanup(url); }
};
using UrlHandle = std::unique_ptr<CURLU, UrlFree>;

struct PartFree {
    void operator()(char* part) const noexcept { curl_free(part); }
};
using Part = std::unique_ptr<char, PartFree>;

std::string lower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

// The URL as libcurl reads it; null when it cannot be read as an absolute URL.
UrlHandle parsed(std::string_view text) {
    if (text.find('\0') != std::string_view::npos) return nullptr;
    UrlHandle url(curl_url());
    if (!url) return nullptr;
    const std::string copy(text);
    if (curl_url_set(url.get(), CURLUPART_URL, copy.c_str(), 0) != CURLUE_OK) return nullptr;
    return url;
}

std::optional<std::string> part(CURLU* url, CURLUPart which, unsigned int flags = 0) {
    char* value = nullptr;
    if (curl_url_get(url, which, &value, flags) != CURLUE_OK || value == nullptr) return std::nullopt;
    const Part owned(value);
    return std::string(owned.get());
}

std::string_view purpose_name(UrlPurpose purpose) {
    switch (purpose) {
        case UrlPurpose::redirect: return "a redirect";
        case UrlPurpose::event_endpoint: return "the endpoint for messages";
        case UrlPurpose::authorization: return "a document for authorization";
    }
    return "a URL";
}

}  // namespace

HostReach reach_of(std::string_view host) {
    if (host.size() >= 2 && host.front() == ipv6_open && host.back() == ipv6_close) {
        std::error_code error;
        const std::string inner(host.substr(1, host.size() - 2));
        const asio::ip::address_v6 address = asio::ip::make_address_v6(inner, error);
        if (error) return HostReach::other;
        if (address.is_loopback()) return HostReach::loopback;
        if (address.is_unspecified()) return HostReach::local;
        if (address.is_v4_mapped()) return reach_of_v4(asio::ip::make_address_v4(asio::ip::v4_mapped, address));
        return HostReach::other;
    }
    std::error_code error;
    const asio::ip::address_v4 address = asio::ip::make_address_v4(std::string(host), error);
    if (!error) return reach_of_v4(address);
    // A name: one trailing dot makes the same absolute name.
    std::string_view name = host;
    if (!name.empty() && name.back() == label_separator) name.remove_suffix(1);
    if (name == loopback_name) return HostReach::loopback;
    // The last label is `localhost`: a name some resolvers answer with a loopback address.
    const auto last_dot = name.rfind(label_separator);
    if (last_dot != std::string_view::npos && name.substr(last_dot + 1) == loopback_name) return HostReach::local;
    return HostReach::other;
}

std::variant<Origin, std::string> origin_of(std::string_view text) {
    const UrlHandle url = parsed(text);
    if (!url) return std::string("the URL cannot be read as an absolute URL");
    const auto scheme = part(url.get(), CURLUPART_SCHEME);
    if (!scheme) return std::string("the URL has no scheme");
    Origin origin;
    origin.scheme = lower(*scheme);
    if (origin.scheme != scheme_http && origin.scheme != scheme_https) {
        return std::string("the URL's scheme is neither http nor https");
    }
    if (part(url.get(), CURLUPART_USER) || part(url.get(), CURLUPART_PASSWORD) || part(url.get(), CURLUPART_OPTIONS)) {
        return std::string("the URL carries user information");
    }
    const auto host = part(url.get(), CURLUPART_HOST);
    if (!host || host->empty()) return std::string("the URL has no host");
    origin.host = lower(*host);
    const auto port = part(url.get(), CURLUPART_PORT, CURLU_DEFAULT_PORT);
    if (!port) return std::string("the URL has no port and its scheme none by default");
    const char* begin = port->data();
    const char* end = begin + port->size();
    const auto [stop, error] = std::from_chars(begin, end, origin.port);
    if (error != std::errc() || stop != end || origin.port == 0) return std::string("the URL's port cannot be read");
    return origin;
}

std::optional<std::string> resolve_reference(std::string_view base, std::string_view reference) {
    if (reference.find('\0') != std::string_view::npos) return std::nullopt;
    const UrlHandle url = parsed(base);
    if (!url) return std::nullopt;
    const std::string copy(reference);
    if (curl_url_set(url.get(), CURLUPART_URL, copy.c_str(), 0) != CURLUE_OK) return std::nullopt;
    return part(url.get(), CURLUPART_URL);
}

UrlGuard::UrlGuard(Origin origin) : origin_(std::move(origin)), reach_(reach_of(origin_.host)) {}

std::variant<UrlGuard, std::string> UrlGuard::for_endpoint(std::string_view endpoint_url) {
    auto origin = origin_of(endpoint_url);
    if (auto* problem = std::get_if<std::string>(&origin)) return "the endpoint cannot be used: " + *problem;
    return UrlGuard(std::get<Origin>(std::move(origin)));
}

bool UrlGuard::carries_tokens() const noexcept {
    return origin_.scheme == scheme_https || reach_ == HostReach::loopback;
}

std::optional<std::string> UrlGuard::admit(UrlPurpose purpose, std::string_view url) const {
    auto read = origin_of(url);
    const auto refused = [purpose](std::string_view why) {
        return std::string(purpose_name(purpose)) + " is refused: " + std::string(why);
    };
    if (auto* problem = std::get_if<std::string>(&read)) return refused(*problem);
    const Origin& origin = std::get<Origin>(read);
    if (purpose != UrlPurpose::authorization) {
        if (origin != origin_) return refused("it leaves the endpoint's origin");
        return std::nullopt;
    }
    const UrlHandle handle = parsed(url);
    if (!handle || part(handle.get(), CURLUPART_FRAGMENT)) return refused("the URL carries a fragment");
    const HostReach reach = reach_of(origin.host);
    if (reach == HostReach::local) return refused("it names this machine by an address or a name that is not loopback");
    if (reach == HostReach::loopback && reach_ != HostReach::loopback) {
        return refused("it names this machine, and the endpoint is elsewhere");
    }
    if (origin.scheme == scheme_http && reach != HostReach::loopback) {
        return refused("it is plain HTTP to a host that is not this machine");
    }
    return std::nullopt;
}

}  // namespace neomcp::detail::http_client
