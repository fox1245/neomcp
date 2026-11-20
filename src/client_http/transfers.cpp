// The transfer thread of an HTTP transport (transfers.h).
#include "client_http/transfers.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

#include <curl/curl.h>

#include "neomcp/http/headers.h"
#include "neomcp/http/media_type.h"
#include "neomcp/http/sse_parser.h"

namespace neomcp::detail::http_client {

namespace {

// The schemes a request and a redirect may use.
constexpr const char* allowed_protocols = "http,https";

constexpr int status_continue_class_end = 200;  // 1xx answers are interim: the final head follows
constexpr int status_success_first = 200;
constexpr int status_success_last = 299;
constexpr int status_accepted = 202;
constexpr int status_no_content = 204;
constexpr int status_temporary_redirect = 307;
constexpr int status_permanent_redirect = 308;

constexpr const char* delete_method = "DELETE";

constexpr std::string_view field_content_type = "Content-Type";
constexpr std::string_view field_location = "Location";
constexpr std::string_view json_type = "application";
constexpr std::string_view json_subtype = "json";
constexpr std::string_view event_stream_type = "text";
constexpr std::string_view event_stream_subtype = "event-stream";

bool is_success(int status) { return status >= status_success_first && status <= status_success_last; }

bool is_token_char(char c) {
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
    constexpr std::string_view others = "!#$%&'*+-.^_`|~";
    return others.find(c) != std::string_view::npos;
}

bool is_whitespace(char c) { return c == ' ' || c == '\t'; }

std::string_view trim(std::string_view text) {
    while (!text.empty() && is_whitespace(text.front())) text.remove_prefix(1);
    while (!text.empty() && is_whitespace(text.back())) text.remove_suffix(1);
    return text;
}

std::string media_name(std::string_view type, std::string_view subtype) {
    std::string name(type);
    name.push_back('/');
    name.append(subtype);
    return name;
}

std::mutex& global_mutex() {
    static std::mutex mutex;
    return mutex;
}
std::size_t global_holders = 0;
bool global_ok = false;

}  // namespace

// ---- libcurl's global state

CurlGlobal::CurlGlobal() {
    std::lock_guard lock(global_mutex());
    if (global_holders == 0) global_ok = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    ++global_holders;
    ok_ = global_ok;
}

CurlGlobal::~CurlGlobal() {
    std::lock_guard lock(global_mutex());
    if (--global_holders == 0 && global_ok) {
        curl_global_cleanup();
        global_ok = false;
    }
}

std::optional<std::string> resolver_refusal(bool asynchronous, bool allow_blocking) {
    if (asynchronous || allow_blocking) return std::nullopt;
    return std::string(
        "the HTTP library resolves host names synchronously, which would stall every exchange of the transport while "
        "a name is resolved; set HttpEndpoint::allow_blocking_resolver to accept that");
}

std::optional<std::string> resolver_refusal(bool allow_blocking) {
    const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    const bool asynchronous = info != nullptr && (info->features & CURL_VERSION_ASYNCHDNS) != 0;
    return resolver_refusal(asynchronous, allow_blocking);
}

// ---- header lines

std::optional<std::string> header_line(std::string_view name, std::string_view value) {
    if (name.empty()) return std::nullopt;
    for (const char c : name) {
        if (!is_token_char(c)) return std::nullopt;
    }
    for (const char c : value) {
        if (c == '\r' || c == '\n' || c == '\0') return std::nullopt;
    }
    std::string line(name);
    if (trim(value).empty()) {
        line.push_back(';');
        return line;
    }
    line.append(": ");
    line.append(value);
    return line;
}

// ---- the body of a response

BodyPlan plan_body(int status, const std::vector<std::string_view>& content_types, long long content_length) {
    BodyPlan plan;
    if (content_types.size() > 1) {
        plan.media_type = std::string();
        return plan;
    }
    if (content_types.empty()) {
        const bool carries_content =
            status != status_accepted && status != status_no_content && content_length != 0;
        if (is_success(status) && carries_content) plan.media_type = std::string();
        return plan;
    }
    const std::optional<http::MediaType> media = http::parse_media_type(content_types.front());
    if (!media) {
        plan.media_type = std::string();
        return plan;
    }
    plan.media_type = media_name(media->type, media->subtype);
    if (media->type == json_type && media->subtype == json_subtype) {
        plan.kind = BodyKind::json;
    } else if (media->type == event_stream_type && media->subtype == event_stream_subtype && is_success(status)) {
        plan.kind = BodyKind::event_stream;
    }
    return plan;
}

// ---- the loop

namespace {

struct EasyFree {
    void operator()(CURL* easy) const noexcept { curl_easy_cleanup(easy); }
};
struct ListFree {
    void operator()(curl_slist* list) const noexcept { curl_slist_free_all(list); }
};

struct Submit {
    TransferRequest request;
};
struct Remove {
    UnitId unit = connection_unit;
};
struct Stop {};
using Command = std::variant<Submit, Remove, Stop>;

enum class Abort {
    none,
    header_bound,  // the response's header fields exceeded their bound
    message_bound  // a message exceeded its bound; it was posted, longer than the bound
};

}  // namespace

struct TransferLoop::Impl {
    using Clock = std::chrono::steady_clock;

    // One exchange. Lives in `transfers` while its handle is in the multi handle.
    struct Transfer {
        Impl* loop = nullptr;
        TransferRequest request;
        std::size_t redirects = 0;
        std::unique_ptr<CURL, EasyFree> easy;
        std::unique_ptr<curl_slist, ListFree> headers;

        std::size_t header_bytes = 0;
        bool expect_status_line = true;
        bool head_done = false;  // the final head was read (and posted, unless it redirects)
        bool head_posted = false;
        std::vector<HttpHeader> fields;
        bool redirecting = false;
        std::string location;
        BodyKind kind = BodyKind::discard;
        std::string body;
        std::optional<http::SseParser> events;
        Abort abort = Abort::none;
        bool idle_bounded = false;  // an event stream has begun: from now on bytes must keep coming
        Clock::time_point last_byte{};
    };

    Impl(TransferLimits limits_in, UrlGuard guard_in, TransferSink sink_in)
        : limits(std::move(limits_in)), guard(std::move(guard_in)), sink(std::move(sink_in)) {
        if (limits.ca_file) ca_file = limits.ca_file->string();
    }

    ~Impl() {
        transfers.clear();
        if (multi != nullptr) curl_multi_cleanup(multi);
    }

    // ---- commands (any thread)

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

    // ---- the thread

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
                    if (!stopping) take(std::move(submit->request));
                } else if (auto* remove = std::get_if<Remove>(&next)) {
                    drop(remove->unit);
                    waiting.erase(remove->unit);
                } else {
                    stopping = true;
                }
            }
            if (stopping) break;
            start_due(Clock::now());
            int still_running = 0;
            curl_multi_perform(multi, &still_running);
            collect();
            expire_idle(Clock::now());
            {
                std::lock_guard lock(mutex);
                if (!commands.empty()) continue;
            }
            curl_multi_poll(multi, nullptr, 0, wait_ms(Clock::now()), nullptr);
        }
        while (!transfers.empty()) drop(transfers.begin()->first);
    }

    void post(UnitId unit, TransferItem item) { sink(unit, std::move(item)); }

    // A submitted request: started now, or kept until its time. Either way it replaces what its unit had.
    void take(TransferRequest request) {
        const UnitId unit = request.unit;
        waiting.erase(unit);
        if (request.not_before > Clock::now()) {
            drop(unit);
            waiting.emplace(unit, std::move(request));
            return;
        }
        add(std::move(request), 0);
    }

    // Starts the requests whose time came.
    void start_due(Clock::time_point now) {
        std::vector<UnitId> due;
        for (const auto& [unit, request] : waiting) {
            if (request.not_before <= now) due.push_back(unit);
        }
        for (const UnitId unit : due) {
            auto found = waiting.find(unit);
            TransferRequest request = std::move(found->second);
            waiting.erase(found);
            add(std::move(request), 0);
        }
    }

    void add(TransferRequest request, std::size_t redirects) {
        const UnitId unit = request.unit;
        auto transfer = std::make_unique<Transfer>();
        transfer->loop = this;
        transfer->request = std::move(request);
        transfer->redirects = redirects;
        if (auto problem = prepare(*transfer)) {
            post(unit, SendFailed{*problem});
            return;
        }
        drop(unit);  // a unit has one exchange at a time
        if (curl_multi_add_handle(multi, transfer->easy.get()) != CURLM_OK) {
            post(unit, SendFailed{"the HTTP library refused the exchange"});
            return;
        }
        transfers.emplace(unit, std::move(transfer));
        count.store(transfers.size());
    }

    // Ends a unit's exchange; nothing is posted for it.
    void drop(UnitId unit) {
        const auto found = transfers.find(unit);
        if (found == transfers.end()) return;
        curl_multi_remove_handle(multi, found->second->easy.get());
        transfers.erase(found);
        count.store(transfers.size());
    }

    std::optional<std::string> prepare(Transfer& transfer) {
        transfer.easy.reset(curl_easy_init());
        if (!transfer.easy) return std::string("the HTTP library could not make a handle");
        for (const std::string& line : transfer.request.header_lines) {
            curl_slist* longer = curl_slist_append(transfer.headers.get(), line.c_str());
            if (longer == nullptr) return std::string("the HTTP library could not take a header field");
            static_cast<void>(transfer.headers.release());
            transfer.headers.reset(longer);
        }
        CURL* easy = transfer.easy.get();
        bool ok = true;
        const auto set = [&ok](CURLcode result) { ok = ok && result == CURLE_OK; };
        set(curl_easy_setopt(easy, CURLOPT_URL, transfer.request.url.c_str()));
        set(curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, allowed_protocols));
        set(curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, allowed_protocols));
        set(curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L));
        set(curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L));
        set(curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(limits.connect_timeout.count())));
        set(curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE, 1L));
        // No bound on the whole exchange unless the request names one (the request that ends a session), and no
        // low-speed abort: the client's clocks decide how long a request waits for its answer, and the loop itself
        // bounds an event stream that has begun and then carries no byte.
        set(curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, static_cast<long>(transfer.request.total_timeout.count())));
        set(curl_easy_setopt(easy, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L));
        switch (limits.proxy.mode) {
            case ProxyMode::environment: break;  // libcurl reads the environment by default
            case ProxyMode::none: set(curl_easy_setopt(easy, CURLOPT_PROXY, "")); break;
            case ProxyMode::named: set(curl_easy_setopt(easy, CURLOPT_PROXY, limits.proxy.url.c_str())); break;
        }
        if (ca_file) set(curl_easy_setopt(easy, CURLOPT_CAINFO, ca_file->c_str()));
        switch (transfer.request.method) {
            case TransferMethod::post:
                set(curl_easy_setopt(easy, CURLOPT_POST, 1L));
                set(curl_easy_setopt(easy, CURLOPT_POSTFIELDS, transfer.request.body.data()));
                set(curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE,
                                     static_cast<curl_off_t>(transfer.request.body.size())));
                break;
            case TransferMethod::get: set(curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L)); break;
            case TransferMethod::remove:
                // A request without a body under another method name; CURLOPT_NOBODY would make it a HEAD.
                set(curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L));
                set(curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, delete_method));
                break;
        }
        set(curl_easy_setopt(easy, CURLOPT_HTTPHEADER, transfer.headers.get()));
        set(curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, &Impl::on_header));
        set(curl_easy_setopt(easy, CURLOPT_HEADERDATA, &transfer));
        set(curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &Impl::on_body));
        set(curl_easy_setopt(easy, CURLOPT_WRITEDATA, &transfer));
        set(curl_easy_setopt(easy, CURLOPT_PRIVATE, &transfer));
        if (!ok) return std::string("the HTTP library refused an option of the exchange");
        return std::nullopt;
    }

    // How long the next wait may last: the idle bound of the loop, shortened to the moment the first event stream that
    // has begun would have been idle for stream_idle_timeout, and to the start of the first request that waits.
    int wait_ms(Clock::time_point now) const {
        auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(limits.idle_wait);
        for (const auto& [unit, request] : waiting) {
            const auto left = std::chrono::ceil<std::chrono::milliseconds>(request.not_before - now);
            if (left < wait) wait = left;
        }
        for (const auto& [unit, transfer] : transfers) {
            if (!transfer->idle_bounded) continue;
            const auto left = std::chrono::ceil<std::chrono::milliseconds>(
                transfer->last_byte + limits.stream_idle_timeout - now);
            if (left < wait) wait = left;
        }
        if (wait.count() < 0) return 0;
        if (wait.count() > std::numeric_limits<int>::max()) return std::numeric_limits<int>::max();
        return static_cast<int>(wait.count());
    }

    // Ends the event streams that have begun and carried no byte, not even a comment, for stream_idle_timeout. A
    // request still waiting for the head of its answer is not bounded here: the client's clock of the request is.
    void expire_idle(Clock::time_point now) {
        std::vector<UnitId> idle;
        for (const auto& [unit, transfer] : transfers) {
            if (transfer->idle_bounded && now - transfer->last_byte >= limits.stream_idle_timeout) idle.push_back(unit);
        }
        for (const UnitId unit : idle) {
            drop(unit);
            post(unit, StreamLost{"the event stream carried no byte for stream_idle_timeout"});
        }
    }

    // ---- callbacks (the thread, inside curl_multi_perform)

    static size_t on_header(char* data, size_t size, size_t count, void* user) {
        Transfer& transfer = *static_cast<Transfer*>(user);
        const std::size_t bytes = size * count;
        Impl& loop = *transfer.loop;
        if (bytes > loop.limits.max_response_header_bytes - transfer.header_bytes) {
            transfer.abort = Abort::header_bound;
            return 0;  // anything but `bytes` aborts the exchange
        }
        transfer.header_bytes += bytes;
        if (transfer.head_done) return bytes;  // trailers after the body
        std::string_view line(data, bytes);
        if (!line.empty() && line.back() == '\n') line.remove_suffix(1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (transfer.expect_status_line) {
            transfer.expect_status_line = false;
            return bytes;
        }
        if (line.empty()) {
            loop.head_complete(transfer);
            transfer.expect_status_line = true;
            return bytes;
        }
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) return bytes;  // a line that is no field
        transfer.fields.push_back(
            HttpHeader{std::string(line.substr(0, colon)), std::string(trim(line.substr(colon + 1)))});
        return bytes;
    }

    void head_complete(Transfer& transfer) {
        long code = 0;
        curl_easy_getinfo(transfer.easy.get(), CURLINFO_RESPONSE_CODE, &code);
        const int status = code > std::numeric_limits<int>::max() ? 0 : static_cast<int>(code);
        if (status < status_continue_class_end) {
            transfer.fields.clear();  // an interim answer; the final head follows
            return;
        }
        transfer.head_done = true;
        if (follows(transfer, status)) return;
        std::vector<std::string_view> types;
        for (const HttpHeader& field : transfer.fields) {
            if (http::equal_ignoring_case(field.name, field_content_type)) types.push_back(field.value);
        }
        curl_off_t length = -1;
        if (curl_easy_getinfo(transfer.easy.get(), CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length) != CURLE_OK) {
            length = -1;
        }
        BodyPlan plan = plan_body(status, types, static_cast<long long>(length));
        if (transfer.request.discard_body) plan.kind = BodyKind::discard;
        transfer.kind = plan.kind;
        if (plan.kind == BodyKind::event_stream) {
            transfer.events.emplace(limits.max_message_bytes);
            transfer.idle_bounded = true;
            transfer.last_byte = Clock::now();
        }
        transfer.head_posted = true;
        post(transfer.request.unit,
             TransferHead{status, std::move(plan.media_type), std::exchange(transfer.fields, {})});
    }

    // Whether the head is a redirect the loop follows: 307 or 308, one Location that resolves to a URL of the
    // endpoint's origin, and the bound not reached. The method and the body are kept; the body of the redirect is
    // dropped.
    bool follows(Transfer& transfer, int status) {
        if (status != status_temporary_redirect && status != status_permanent_redirect) return false;
        if (transfer.redirects >= limits.max_redirects) return false;
        const HttpHeader* location = nullptr;
        for (const HttpHeader& field : transfer.fields) {
            if (!http::equal_ignoring_case(field.name, field_location)) continue;
            if (location != nullptr) return false;
            location = &field;
        }
        if (location == nullptr) return false;
        auto target = resolve_reference(transfer.request.url, location->value);
        if (!target || guard.admit(UrlPurpose::redirect, *target)) return false;
        transfer.redirecting = true;
        transfer.location = std::move(*target);
        return true;
    }

    static size_t on_body(char* data, size_t size, size_t count, void* user) {
        Transfer& transfer = *static_cast<Transfer*>(user);
        const std::size_t bytes = size * count;
        if (transfer.redirecting || transfer.kind == BodyKind::discard) return bytes;
        Impl& loop = *transfer.loop;
        const std::size_t bound = loop.limits.max_message_bytes;
        const UnitId unit = transfer.request.unit;
        if (transfer.idle_bounded) transfer.last_byte = Clock::now();
        if (transfer.kind == BodyKind::json) {
            if (bytes > bound - transfer.body.size()) {
                // Keep one byte beyond the bound, so that the engine sees a message too long and ends the call.
                transfer.body.append(data, bound - transfer.body.size() + 1);
                transfer.abort = Abort::message_bound;
                loop.post(unit, TransferMessage{std::exchange(transfer.body, {})});
                return CURL_WRITEFUNC_ERROR;
            }
            transfer.body.append(data, bytes);
            return bytes;
        }
        const auto status = transfer.events->feed(std::string_view(data, bytes), [&](http::SseItem item) {
            if (auto* event = std::get_if<http::SseEvent>(&item)) {
                if (event->data.empty()) return;
                if (transfer.request.typed_events) {
                    loop.post(unit, TransferEvent{std::move(event->type), std::move(event->data)});
                } else if (event->type == http::sse_default_event_type) {
                    loop.post(unit, TransferMessage{std::move(event->data)});
                }
            } else if (auto* id = std::get_if<http::SseLastEventId>(&item)) {
                loop.post(unit, StreamEventId{std::move(id->id)});
            } else {
                loop.post(unit, StreamRetryAdvice{std::get<http::SseRetry>(item).delay});
            }
        });
        if (status == http::SseParser::Status::too_large) {
            transfer.abort = Abort::message_bound;
            if (transfer.request.typed_events) {
                loop.post(unit, TransferEvent{std::string(), transfer.events->take_oversized()});
            } else {
                loop.post(unit, TransferMessage{transfer.events->take_oversized()});
            }
            return CURL_WRITEFUNC_ERROR;
        }
        return bytes;
    }

    // ---- ends of exchanges

    void collect() {
        int left = 0;
        while (CURLMsg* message = curl_multi_info_read(multi, &left)) {
            if (message->msg != CURLMSG_DONE) continue;
            CURL* easy = message->easy_handle;
            const CURLcode result = message->data.result;
            char* private_data = nullptr;
            curl_easy_getinfo(easy, CURLINFO_PRIVATE, &private_data);
            auto* transfer = reinterpret_cast<Transfer*>(private_data);
            const auto found = transfers.find(transfer->request.unit);
            if (found == transfers.end() || found->second.get() != transfer) continue;
            std::unique_ptr<Transfer> owned = std::move(found->second);
            transfers.erase(found);
            curl_multi_remove_handle(multi, easy);
            count.store(transfers.size());
            finish(*owned, result);
        }
    }

    void finish(Transfer& transfer, CURLcode result) {
        const UnitId unit = transfer.request.unit;
        switch (transfer.abort) {
            case Abort::message_bound: return;  // the message too long was posted; the engine ends the call
            case Abort::header_bound:
                post(unit, StreamLost{"the response's header fields exceed max_response_header_bytes"});
                return;
            case Abort::none: break;
        }
        if (result == CURLE_OK && transfer.redirecting) {
            TransferRequest again = std::move(transfer.request);
            again.url = std::move(transfer.location);
            add(std::move(again), transfer.redirects + 1);
            return;
        }
        if (result == CURLE_OK) {
            if (transfer.kind == BodyKind::json && !transfer.body.empty()) {
                post(unit, TransferMessage{std::move(transfer.body)});
            }
            if (transfer.events) transfer.events->finish();
            post(unit, StreamEnded{});
            return;
        }
        long request_bytes = 0;
        if (curl_easy_getinfo(transfer.easy.get(), CURLINFO_REQUEST_SIZE, &request_bytes) != CURLE_OK) {
            request_bytes = 1;
        }
        std::string cause = curl_easy_strerror(result);
        if (!transfer.head_posted && request_bytes == 0) {
            post(unit, SendFailed{std::move(cause)});  // nothing reached the server
        } else {
            post(unit, StreamLost{std::move(cause)});
        }
    }

    TransferLimits limits;
    UrlGuard guard;
    TransferSink sink;
    std::optional<std::string> ca_file;
    CURLM* multi = nullptr;

    std::mutex mutex;  // guards `commands` and `closed`, and every use of `multi` from another thread
    std::deque<Command> commands;
    bool closed = false;

    std::map<UnitId, std::unique_ptr<Transfer>> transfers;  // the thread's alone
    std::map<UnitId, TransferRequest> waiting;               // the thread's alone: requests not yet due
    std::atomic<std::size_t> count{0};
};

TransferLoop::TransferLoop(TransferLimits limits, UrlGuard guard, TransferSink sink)
    : impl_(std::make_shared<Impl>(std::move(limits), std::move(guard), std::move(sink))) {}

TransferLoop::~TransferLoop() { stop(); }

std::optional<std::string> TransferLoop::start() {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->multi != nullptr || impl_->closed) throw std::logic_error("a transfer loop was started twice");
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

void TransferLoop::submit(TransferRequest request) { impl_->command(Submit{std::move(request)}); }

void TransferLoop::remove(UnitId unit) { impl_->command(Remove{unit}); }

void TransferLoop::stop() {
    {
        std::lock_guard lock(stop_mutex_);
        if (stopped_) return;
        stopped_ = true;
    }
    impl_->request_stop();
    if (thread_.joinable()) thread_.join();
}

std::size_t TransferLoop::running() const { return impl_->count.load(); }

}  // namespace neomcp::detail::http_client
