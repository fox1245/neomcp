// The fetches of the JWT validator (fetch.h).
#include "server_auth_jwt/fetch.h"

#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include <curl/curl.h>

#include "neomcp/http/policy.h"

namespace neomcp::http::detail {

namespace {

constexpr std::string_view scheme_http = "http";
constexpr std::string_view scheme_https = "https";
// The schemes a request may use; a redirect is never followed.
constexpr const char* allowed_protocols = "http,https";
constexpr const char* no_proxy = "";
constexpr const char* user_agent = "neomcp";
// The media types of a key set (RFC 7517, section 8.5.1) and of metadata.
constexpr const char* header_accept = "Accept: application/jwk-set+json, application/json";
// The path segment of the metadata (RFC 8414, section 3.1).
constexpr std::string_view metadata_well_known_path = "/.well-known/oauth-authorization-server";
constexpr char path_separator = '/';
// The longest the thread waits in one call to curl_multi_poll; interrupt() wakes it at once.
constexpr int idle_wait_ms = 1000;

struct UrlFree {
    void operator()(CURLU* url) const noexcept { curl_url_cleanup(url); }
};
struct PartFree {
    void operator()(char* part) const noexcept { curl_free(part); }
};
struct EasyFree {
    void operator()(CURL* easy) const noexcept { curl_easy_cleanup(easy); }
};
struct ListFree {
    void operator()(curl_slist* list) const noexcept { curl_slist_free_all(list); }
};
using Url = std::unique_ptr<CURLU, UrlFree>;

// The part of a URL as libcurl, which makes the request, reads it; nothing when the URL has none.
std::optional<std::string> url_part(CURLU* url, CURLUPart which) {
    char* value = nullptr;
    if (curl_url_get(url, which, &value, 0) != CURLUE_OK || value == nullptr) return std::nullopt;
    const std::unique_ptr<char, PartFree> owned(value);
    return std::string(owned.get());
}

// The URL read by libcurl, or a text that says why it cannot be fetched from.
std::variant<Url, std::string> read_url(std::string_view text, std::string_view what) {
    const std::string name(what);
    if (text.empty()) return name + " is empty";
    if (text.find('\0') != std::string_view::npos) return name + " cannot be read as a URL";
    Url url(curl_url());
    const std::string owned(text);
    if (!url || curl_url_set(url.get(), CURLUPART_URL, owned.c_str(), 0) != CURLUE_OK) {
        return name + " cannot be read as an absolute URL";
    }
    const auto scheme = url_part(url.get(), CURLUPART_SCHEME);
    if (!scheme || (*scheme != scheme_http && *scheme != scheme_https)) {
        return name + " must be an https URL, or an http URL with a loopback host";
    }
    if (url_part(url.get(), CURLUPART_USER) || url_part(url.get(), CURLUPART_PASSWORD) ||
        url_part(url.get(), CURLUPART_OPTIONS)) {
        return name + " must have no user information";
    }
    if (url_part(url.get(), CURLUPART_FRAGMENT)) return name + " must have no fragment";
    auto host = url_part(url.get(), CURLUPART_HOST);
    if (!host || host->empty()) return name + " must have a host";
    if (*scheme == scheme_http) {
        // libcurl writes an IPv6 address in brackets.
        if (host->size() >= 2 && host->front() == '[' && host->back() == ']') *host = host->substr(1, host->size() - 2);
        if (!is_loopback_address(*host)) return name + " must be an https URL, or an http URL with a loopback host";
    }
    return std::variant<Url, std::string>(std::move(url));
}

enum class Abort { none, body_bound, header_bound };

}  // namespace

std::string fetch_url_problem(std::string_view url, std::string_view what) {
    auto read = read_url(url, what);
    if (auto* problem = std::get_if<std::string>(&read)) return std::move(*problem);
    return {};
}

std::optional<std::string> metadata_url_of(std::string_view issuer) {
    auto read = read_url(issuer, "the issuer");
    auto* url = std::get_if<Url>(&read);
    if (url == nullptr) return std::nullopt;
    if (url_part(url->get(), CURLUPART_QUERY)) return std::nullopt;
    std::string path = url_part(url->get(), CURLUPART_PATH).value_or(std::string());
    if (!path.empty() && path.back() == path_separator) path.pop_back();
    const std::string well_known = std::string(metadata_well_known_path) + path;
    if (curl_url_set(url->get(), CURLUPART_PATH, well_known.c_str(), 0) != CURLUE_OK) return std::nullopt;
    return url_part(url->get(), CURLUPART_URL);
}

std::optional<std::string> key_set_resolver_problem(bool allow_blocking) {
    const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    const bool asynchronous = info != nullptr && (info->features & CURL_VERSION_ASYNCHDNS) != 0;
    if (asynchronous || allow_blocking) return std::nullopt;
    return std::string(
        "the HTTP library resolves host names synchronously, so a fetch of the key set could not be stopped while a "
        "name is resolved; set allow_blocking_resolver to accept that");
}

struct Fetcher::Impl {
    struct Transfer {
        const FetchRequest* request = nullptr;
        std::string body;
        std::size_t header_bytes = 0;
        Abort abort = Abort::none;
    };

    ~Impl() {
        if (multi != nullptr) curl_multi_cleanup(multi);
        if (global) curl_global_cleanup();
    }

    static size_t on_header(char*, size_t size, size_t count, void* user) {
        Transfer& transfer = *static_cast<Transfer*>(user);
        const std::size_t bytes = size * count;
        if (bytes > transfer.request->max_header_bytes - transfer.header_bytes) {
            transfer.abort = Abort::header_bound;
            return 0;  // anything but `bytes` ends the transfer
        }
        transfer.header_bytes += bytes;
        return bytes;
    }

    static size_t on_body(char* data, size_t size, size_t count, void* user) {
        Transfer& transfer = *static_cast<Transfer*>(user);
        const std::size_t bytes = size * count;
        if (bytes > transfer.request->max_body_bytes - transfer.body.size()) {
            transfer.abort = Abort::body_bound;
            return 0;
        }
        transfer.body.append(data, bytes);
        return bytes;
    }

    bool prepare(CURL* easy, curl_slist* headers, Transfer& transfer) {
        const FetchRequest& request = *transfer.request;
        bool ok = true;
        const auto set = [&ok](CURLcode result) { ok = ok && result == CURLE_OK; };
        set(curl_easy_setopt(easy, CURLOPT_URL, request.url.c_str()));
        set(curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, allowed_protocols));
        set(curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, allowed_protocols));
        set(curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L));
        set(curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L));
        set(curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(request.connect_timeout.count())));
        set(curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, static_cast<long>(request.total_timeout.count())));
        set(curl_easy_setopt(easy, CURLOPT_USERAGENT, user_agent));
        set(curl_easy_setopt(easy, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L));
        if (!request.use_proxy_from_environment) set(curl_easy_setopt(easy, CURLOPT_PROXY, no_proxy));
        if (request.ca_file) set(curl_easy_setopt(easy, CURLOPT_CAINFO, request.ca_file->c_str()));
        // A body announced to be longer than the bound ends the transfer before it is read.
        set(curl_easy_setopt(easy, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(request.max_body_bytes)));
        set(curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L));
        set(curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers));
        set(curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, &Impl::on_header));
        set(curl_easy_setopt(easy, CURLOPT_HEADERDATA, &transfer));
        set(curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &Impl::on_body));
        set(curl_easy_setopt(easy, CURLOPT_WRITEDATA, &transfer));
        return ok;
    }

    CURLM* multi = nullptr;
    bool global = false;
    std::mutex mutex;  // guards `interrupted` and every use of `multi` from another thread
    bool interrupted = false;
};

Fetcher::Fetcher() : impl_(std::make_unique<Impl>()) {}

Fetcher::~Fetcher() = default;

std::optional<std::string> Fetcher::start() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->multi != nullptr) throw std::logic_error("a fetcher was started twice");
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return std::string("the HTTP library could not be initialized");
    impl_->global = true;
    impl_->multi = curl_multi_init();
    if (impl_->multi == nullptr) return std::string("the HTTP library could not make a multi handle");
    return std::nullopt;
}

void Fetcher::interrupt() {
    std::lock_guard lock(impl_->mutex);
    impl_->interrupted = true;
    if (impl_->multi != nullptr) curl_multi_wakeup(impl_->multi);
}

FetchResult Fetcher::fetch(const FetchRequest& request) {
    FetchResult result;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->interrupted) {
            result.end = FetchEnd::interrupted;
            return result;
        }
    }
    const std::unique_ptr<CURL, EasyFree> easy(curl_easy_init());
    std::unique_ptr<curl_slist, ListFree> headers(curl_slist_append(nullptr, header_accept));
    Impl::Transfer transfer;
    transfer.request = &request;
    if (!easy || !headers || !impl_->prepare(easy.get(), headers.get(), transfer) ||
        curl_multi_add_handle(impl_->multi, easy.get()) != CURLM_OK) {
        return result;
    }
    CURLcode code = CURLE_OK;
    bool done = false;
    bool stopped = false;
    while (!done) {
        {
            std::lock_guard lock(impl_->mutex);
            stopped = impl_->interrupted;
        }
        if (stopped) break;
        int still_running = 0;
        curl_multi_perform(impl_->multi, &still_running);
        int left = 0;
        while (CURLMsg* message = curl_multi_info_read(impl_->multi, &left)) {
            if (message->msg == CURLMSG_DONE && message->easy_handle == easy.get()) {
                code = message->data.result;
                done = true;
            }
        }
        if (!done) curl_multi_poll(impl_->multi, nullptr, 0, idle_wait_ms, nullptr);
    }
    long status = 0;
    curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &status);
    curl_multi_remove_handle(impl_->multi, easy.get());
    if (stopped) {
        result.end = FetchEnd::interrupted;
    } else if (code == CURLE_OK && transfer.abort == Abort::none) {
        result.end = FetchEnd::response;
        result.status = status > (std::numeric_limits<int>::max)() ? 0 : static_cast<int>(status);
        result.body = std::move(transfer.body);
    } else if (transfer.abort != Abort::none || code == CURLE_FILESIZE_EXCEEDED) {
        result.end = FetchEnd::too_large;
    }
    return result;
}

}  // namespace neomcp::http::detail
