// The fetches of the JWT validator: the rule for the URLs it fetches from, the well-known URL of an issuer's metadata,
// and one GET at a time on libcurl, which another thread can interrupt. Internal to neomcp::server_auth_jwt.
#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace neomcp::http::detail {

// What is wrong with a URL the validator fetches from; empty when nothing. It must be an https URL, or an http URL
// with a loopback host, with a host, without user information and without a fragment. `what` names the URL in the
// text; the text never holds the URL.
std::string fetch_url_problem(std::string_view url, std::string_view what);

// The URL of the authorization server metadata of `issuer` (RFC 8414, section 3.1): the issuer's scheme and authority,
// then /.well-known/oauth-authorization-server, then the issuer's path without a terminating '/'. Nothing when the
// issuer is not a URL fetch_url_problem accepts, or has a query.
std::optional<std::string> metadata_url_of(std::string_view issuer);

// Why the HTTP library cannot serve (its resolver blocks and the application did not accept that); empty when it can.
std::optional<std::string> key_set_resolver_problem(bool allow_blocking);

struct FetchRequest {
    std::string url;
    std::chrono::milliseconds total_timeout{0};
    std::chrono::milliseconds connect_timeout{0};
    std::size_t max_body_bytes = 0;
    std::size_t max_header_bytes = 0;
    bool use_proxy_from_environment = false;
    std::optional<std::string> ca_file;
};

enum class FetchEnd {
    response,     // a complete response; status and body are set
    too_large,    // the body, or the header fields, exceeded their bound (also when announced in advance)
    failed,       // anything else: not reachable, timed out, a transfer error
    interrupted   // interrupt() was called
};

struct FetchResult {
    FetchEnd end = FetchEnd::failed;
    int status = 0;
    std::string body;
};

// One GET at a time, on the thread that calls fetch(). Model: the transfer runs on a libcurl multi handle that the
// fetching thread waits on; interrupt() wakes it with curl_multi_wakeup and every later fetch() ends at once.
class Fetcher {
public:
    Fetcher();
    ~Fetcher();
    Fetcher(const Fetcher&) = delete;
    Fetcher& operator=(const Fetcher&) = delete;

    // Prepares the HTTP library; why it could not, otherwise. Called once, before fetch().
    std::optional<std::string> start();
    // The fetching thread. Blocks until the transfer ends.
    FetchResult fetch(const FetchRequest& request);
    // Any thread. Ends the transfer in progress and every later one. Idempotent.
    void interrupt();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neomcp::http::detail
