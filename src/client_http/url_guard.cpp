// The guard of URLs a server supplies (url_guard.h).
#include "client_http/url_guard.h"

#include <charconv>
#include <memory>
#include <system_error>

#include <curl/curl.h>

namespace neomcp::detail::http_client {

namespace {

constexpr std::string_view scheme_http = "http";
constexpr std::string_view scheme_https = "https";

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

std::variant<UrlGuard, std::string> UrlGuard::for_endpoint(std::string_view endpoint_url) {
    auto origin = origin_of(endpoint_url);
    if (auto* problem = std::get_if<std::string>(&origin)) return "the endpoint cannot be used: " + *problem;
    return UrlGuard(std::get<Origin>(std::move(origin)));
}

std::optional<std::string> UrlGuard::admit(UrlPurpose purpose, std::string_view url) const {
    auto origin = origin_of(url);
    if (auto* problem = std::get_if<std::string>(&origin)) {
        return std::string(purpose_name(purpose)) + " is refused: " + *problem;
    }
    if (std::get<Origin>(origin) != origin_) {
        return std::string(purpose_name(purpose)) + " is refused: it leaves the endpoint's origin";
    }
    return std::nullopt;
}

}  // namespace neomcp::detail::http_client
