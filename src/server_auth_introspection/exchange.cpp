// The loop of the introspection requests (exchange.h).
#include "server_auth_introspection/exchange.h"

#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <variant>

#include <curl/curl.h>

namespace neomcp::http::detail {

namespace {

// The schemes a request may use; a redirect is never followed.
constexpr const char* allowed_protocols = "http,https";
constexpr const char* no_proxy = "";
constexpr const char* user_agent = "neomcp";
// The longest the thread waits in one call to curl_multi_poll; a command wakes it at once.
constexpr int idle_wait_ms = 1000;

struct EasyFree {
    void operator()(CURL* easy) const noexcept { curl_easy_cleanup(easy); }
};
struct ListFree {
    void operator()(curl_slist* list) const noexcept { curl_slist_free_all(list); }
};

struct Submit {
    ExchangeRequest request;
    ExchangeDone done;
};
struct Stop {};
using Command = std::variant<Submit, Stop>;

enum class Abort { none, body_bound, header_bound };

}  // namespace

std::optional<std::string> exchange_resolver_problem(bool allow_blocking) {
    const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    const bool asynchronous = info != nullptr && (info->features & CURL_VERSION_ASYNCHDNS) != 0;
    if (asynchronous || allow_blocking) return std::nullopt;
    return std::string(
        "the HTTP library resolves host names synchronously, which would hold every request of the validator while "
        "a name is resolved; set allow_blocking_resolver to accept that");
}

struct ExchangeLoop::Impl {
    struct Exchange {
        Impl* loop = nullptr;
        ExchangeRequest request;
        ExchangeDone done;
        std::unique_ptr<CURL, EasyFree> easy;
        std::unique_ptr<curl_slist, ListFree> headers;
        std::string body;
        std::size_t header_bytes = 0;
        Abort abort = Abort::none;
    };

    ~Impl() {
        exchanges.clear();
        if (multi != nullptr) curl_multi_cleanup(multi);
        if (global) curl_global_cleanup();
    }

    void command(Command next) {
        std::lock_guard lock(mutex);
        if (closed) return;
        commands.push_back(std::move(next));
        if (multi != nullptr) curl_multi_wakeup(multi);
    }

    void request_stop() {
        std::lock_guard lock(mutex);
        if (closed) return;
        commands.push_back(Stop{});
        if (multi != nullptr) curl_multi_wakeup(multi);
        // No wakeup may reach the multi handle after this: the thread frees it once it has stopped.
        closed = true;
    }

    void run() {
        while (true) {
            std::deque<Command> batch;
            {
                std::lock_guard lock(mutex);
                batch.swap(commands);
            }
            bool stopping = false;
            for (Command& next : batch) {
                if (auto* submit = std::get_if<Submit>(&next)) {
                    if (!stopping) take(std::move(*submit));
                } else {
                    stopping = true;
                }
            }
            if (stopping) break;
            int still_running = 0;
            curl_multi_perform(multi, &still_running);
            collect();
            {
                std::lock_guard lock(mutex);
                if (!commands.empty()) continue;
            }
            curl_multi_poll(multi, nullptr, 0, idle_wait_ms, nullptr);
        }
        for (auto& [easy, exchange] : exchanges) curl_multi_remove_handle(multi, easy);
        exchanges.clear();
    }

    // A request taken from the queue: started, or ended at once when it cannot be.
    void take(Submit submit) {
        auto exchange = std::make_unique<Exchange>();
        exchange->loop = this;
        exchange->request = std::move(submit.request);
        exchange->done = std::move(submit.done);
        if (!prepare(*exchange) || curl_multi_add_handle(multi, exchange->easy.get()) != CURLM_OK) {
            ExchangeDone done = std::move(exchange->done);
            done(ExchangeResult{ExchangeEnd::failed, 0, {}});
            return;
        }
        CURL* key = exchange->easy.get();
        exchanges.emplace(key, std::move(exchange));
    }

    bool prepare(Exchange& exchange) {
        exchange.easy.reset(curl_easy_init());
        if (!exchange.easy) return false;
        for (const std::string& line : exchange.request.header_lines) {
            curl_slist* longer = curl_slist_append(exchange.headers.get(), line.c_str());
            if (longer == nullptr) return false;
            static_cast<void>(exchange.headers.release());
            exchange.headers.reset(longer);
        }
        CURL* easy = exchange.easy.get();
        bool ok = true;
        const auto set = [&ok](CURLcode result) { ok = ok && result == CURLE_OK; };
        const ExchangeRequest& request = exchange.request;
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
        set(curl_easy_setopt(easy, CURLOPT_POST, 1L));
        set(curl_easy_setopt(easy, CURLOPT_POSTFIELDS, request.body.data()));
        set(curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size())));
        set(curl_easy_setopt(easy, CURLOPT_HTTPHEADER, exchange.headers.get()));
        set(curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, &Impl::on_header));
        set(curl_easy_setopt(easy, CURLOPT_HEADERDATA, &exchange));
        set(curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &Impl::on_body));
        set(curl_easy_setopt(easy, CURLOPT_WRITEDATA, &exchange));
        set(curl_easy_setopt(easy, CURLOPT_PRIVATE, &exchange));
        return ok;
    }

    static size_t on_header(char*, size_t size, size_t count, void* user) {
        Exchange& exchange = *static_cast<Exchange*>(user);
        const std::size_t bytes = size * count;
        if (bytes > exchange.request.max_header_bytes - exchange.header_bytes) {
            exchange.abort = Abort::header_bound;
            return 0;  // anything but `bytes` ends the transfer
        }
        exchange.header_bytes += bytes;
        return bytes;
    }

    static size_t on_body(char* data, size_t size, size_t count, void* user) {
        Exchange& exchange = *static_cast<Exchange*>(user);
        const std::size_t bytes = size * count;
        if (bytes > exchange.request.max_body_bytes - exchange.body.size()) {
            exchange.abort = Abort::body_bound;
            return 0;
        }
        exchange.body.append(data, bytes);
        return bytes;
    }

    void collect() {
        int left = 0;
        while (CURLMsg* message = curl_multi_info_read(multi, &left)) {
            if (message->msg != CURLMSG_DONE) continue;
            CURL* easy = message->easy_handle;
            const CURLcode result = message->data.result;
            const auto found = exchanges.find(easy);
            if (found == exchanges.end()) continue;
            std::unique_ptr<Exchange> owned = std::move(found->second);
            exchanges.erase(found);
            long status = 0;
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
            curl_multi_remove_handle(multi, easy);
            ExchangeResult outcome;
            if (result == CURLE_OK && owned->abort == Abort::none) {
                outcome.end = ExchangeEnd::response;
                outcome.status = status > (std::numeric_limits<int>::max)() ? 0 : static_cast<int>(status);
                outcome.body = std::move(owned->body);
            } else if (owned->abort != Abort::none || result == CURLE_FILESIZE_EXCEEDED) {
                outcome.end = ExchangeEnd::body_too_large;
            }
            ExchangeDone done = std::move(owned->done);
            owned.reset();
            done(std::move(outcome));
        }
    }

    CURLM* multi = nullptr;
    bool global = false;
    std::mutex mutex;  // guards `commands`, `closed` and every use of `multi` from another thread
    std::deque<Command> commands;
    bool closed = false;
    std::map<CURL*, std::unique_ptr<Exchange>> exchanges;  // the thread's alone
};

ExchangeLoop::ExchangeLoop() : impl_(std::make_shared<Impl>()) {}

ExchangeLoop::~ExchangeLoop() { stop(); }

std::optional<std::string> ExchangeLoop::start() {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->multi != nullptr || impl_->closed) throw std::logic_error("an exchange loop was started twice");
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            return std::string("the HTTP library could not be initialized");
        }
        impl_->global = true;
        impl_->multi = curl_multi_init();
        if (impl_->multi == nullptr) return std::string("the HTTP library could not make a multi handle");
    }
    try {
        thread_ = std::thread([impl = impl_] { impl->run(); });
    } catch (const std::system_error&) {
        return std::string("the transfer thread could not be started");
    }
    return std::nullopt;
}

void ExchangeLoop::submit(ExchangeRequest request, ExchangeDone done) {
    impl_->command(Submit{std::move(request), std::move(done)});
}

void ExchangeLoop::stop() {
    {
        std::lock_guard lock(stop_mutex_);
        if (stopped_) return;
        stopped_ = true;
    }
    impl_->request_stop();
    if (thread_.joinable()) thread_.join();
}

}  // namespace neomcp::http::detail
