// Shared by the tests of the Client: a timer service the test fires by hand, bounded waits, and peers of the in-memory
// transport that a test scripts.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "neomcp/client.h"
#include "neomcp/client/memory_transport.h"
#include "neomcp/jsonrpc.h"

namespace client_test {

using neomcp::json;

inline constexpr auto wait_bound = std::chrono::seconds(20);

// Waits until `done` holds, for at most the bound; whether it held. It looks again every millisecond.
inline bool eventually(const std::function<bool()>& done, std::chrono::milliseconds bound = wait_bound) {
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) return done();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// Runs its work when it goes: a test closes its client and joins its threads this way even when an assertion ends it
// early. Declared after what it must outlast, it runs before that goes.
class OnExit {
public:
    explicit OnExit(std::function<void()> work) : work_(std::move(work)) {}
    OnExit(const OnExit&) = delete;
    OnExit& operator=(const OnExit&) = delete;
    ~OnExit() {
        if (work_) work_();
    }

private:
    std::function<void()> work_;
};

inline void join(std::thread& thread) {
    if (thread.joinable()) thread.join();
}

// Options that enable exactly these revisions.
inline neomcp::ClientOptions only(std::initializer_list<std::string_view> revisions) {
    neomcp::ClientOptions options;
    for (const auto revision : revisions) options.profiles.push_back(neomcp::profile::builtin(revision));
    options.client_info.name = "client-test";
    options.client_info.version = "1.0.0";
    return options;
}

// A timer service whose timers fire only when the test fires them; its clock moves only when the test moves it.
class ManualTimers final : public neomcp::TimerService {
public:
    struct Started {
        neomcp::TimerId id;
        std::chrono::milliseconds after{0};
        neomcp::TimerPurpose purpose;
        bool cancelled = false;
        bool fired = false;
    };

    std::chrono::steady_clock::time_point now() const override {
        std::lock_guard lock(mutex_);
        return now_;
    }

    neomcp::TimerId start(std::chrono::milliseconds after, neomcp::TimerPurpose purpose,
                          std::function<void()> fire) override {
        std::lock_guard lock(mutex_);
        const neomcp::TimerId id{next_++};
        started_.push_back(Started{id, after, std::move(purpose), false, false});
        fires_.emplace(id, std::move(fire));
        return id;
    }

    void cancel(neomcp::TimerId timer) override {
        std::lock_guard lock(mutex_);
        for (auto& one : started_) {
            if (one.id == timer) one.cancelled = true;
        }
        fires_.erase(timer);
    }

    void advance(std::chrono::milliseconds by) {
        std::lock_guard lock(mutex_);
        now_ += by;
    }

    std::vector<Started> started() const {
        std::lock_guard lock(mutex_);
        return started_;
    }

    // The timers of one kind of purpose, in the order they were started.
    template <class P>
    std::vector<Started> started_for() const {
        std::vector<Started> found;
        for (const auto& one : started()) {
            if (std::holds_alternative<P>(one.purpose)) found.push_back(one);
        }
        return found;
    }

    // The first live timer that `match` takes, once one is started; empty after the bound.
    std::optional<Started> live(const std::function<bool(const Started&)>& match) const {
        std::optional<Started> found;
        eventually([&] {
            std::lock_guard lock(mutex_);
            for (const auto& one : started_) {
                if (!one.cancelled && !one.fired && fires_.count(one.id) != 0 && match(one)) {
                    found = one;
                    return true;
                }
            }
            return false;
        });
        return found;
    }

    // Fires every live timer once (a test that failed early lets its client's clocks lapse this way).
    void fire_all() {
        std::vector<neomcp::TimerId> ids;
        {
            std::lock_guard lock(mutex_);
            for (const auto& [id, work] : fires_) ids.push_back(id);
        }
        for (const auto id : ids) fire(id);
    }

    // Fires a live timer; false when it was cancelled or fired.
    bool fire(neomcp::TimerId timer) {
        std::function<void()> fire;
        {
            std::lock_guard lock(mutex_);
            const auto found = fires_.find(timer);
            if (found == fires_.end()) return false;
            fire = std::move(found->second);
            fires_.erase(found);
            for (auto& one : started_) {
                if (one.id == timer) one.fired = true;
            }
        }
        fire();
        return true;
    }

private:
    mutable std::mutex mutex_;
    std::chrono::steady_clock::time_point now_{};
    std::uint64_t next_ = 1;
    std::vector<Started> started_;
    std::map<neomcp::TimerId, std::function<void()>> fires_;
};

// A request the client wrote, parsed.
struct Seen {
    neomcp::UnitId unit = neomcp::connection_unit;
    json message;
    std::vector<neomcp::HttpHeader> headers;
    std::string method() const { return message.contains("method") ? message["method"].get<std::string>() : ""; }
};

// A peer the test scripts: `on_request` gets each request of the client and answers it through the link (or not);
// notifications and responses are recorded. Every call is recorded with the thread it came on.
class ScriptedPeer final : public neomcp::MemoryPeer {
public:
    using OnRequest = std::function<void(ScriptedPeer& peer, const Seen& request)>;

    explicit ScriptedPeer(OnRequest on_request) : on_request_(std::move(on_request)) {}

    std::optional<std::string> attach(neomcp::MemoryLink link) override {
        std::lock_guard lock(mutex_);
        link_ = std::move(link);
        threads_.push_back(std::this_thread::get_id());
        return std::nullopt;
    }

    void receive(neomcp::Outgoing outgoing) override {
        std::optional<Seen> request;
        {
            std::lock_guard lock(mutex_);
            threads_.push_back(std::this_thread::get_id());
            kinds_.push_back(outgoing.kind);
            if (outgoing.kind == neomcp::OutgoingKind::restart) ++restarts_;
            if (outgoing.line.empty()) {
                if (outgoing.kind == neomcp::OutgoingKind::restart && link_) link_->restarted();
                return;
            }
            Seen seen{outgoing.unit, json::parse(outgoing.line), outgoing.headers};
            written_.push_back(seen);
            if (outgoing.kind == neomcp::OutgoingKind::request) request = seen;
        }
        if (request && on_request_) on_request_(*this, *request);
    }

    void detach(neomcp::CloseCause) override {
        std::lock_guard lock(mutex_);
        detached_ = true;
    }

    // ---- What the script does

    void answer(const Seen& request, json result) {
        send(request.unit, neomcp::serialize(neomcp::make_result(id_of(request), std::move(result))));
        end(request.unit);
    }
    void refuse(const Seen& request, int code, std::string message) {
        send(request.unit, neomcp::serialize(neomcp::make_error(id_of(request), code, std::move(message))));
        end(request.unit);
    }
    void event(neomcp::UnitId unit, neomcp::StreamEvent event) { link().event(unit, std::move(event)); }
    void send(neomcp::UnitId unit, std::string line) { link().message(unit, std::move(line)); }

    // ---- What the test reads

    std::size_t count(std::string_view method) const {
        std::lock_guard lock(mutex_);
        std::size_t found = 0;
        for (const auto& one : written_) found += one.method() == method ? 1 : 0;
        return found;
    }
    std::vector<Seen> written() const {
        std::lock_guard lock(mutex_);
        return written_;
    }
    std::vector<std::thread::id> threads() const {
        std::lock_guard lock(mutex_);
        return threads_;
    }
    // How many outputs of `kind` the client wrote.
    std::size_t count_kind(neomcp::OutgoingKind kind) const {
        std::lock_guard lock(mutex_);
        std::size_t found = 0;
        for (const auto one : kinds_) found += one == kind ? 1 : 0;
        return found;
    }
    std::size_t restarts() const {
        std::lock_guard lock(mutex_);
        return restarts_;
    }
    bool detached() const {
        std::lock_guard lock(mutex_);
        return detached_;
    }

    // stdio: units are not ended; HTTP: the unit ends with its answer.
    bool http = false;

private:
    static neomcp::RequestId id_of(const Seen& request) {
        return neomcp::RequestId(request.message["id"].get<std::uint64_t>());
    }
    void end(neomcp::UnitId unit) {
        if (http) link().event(unit, neomcp::StreamEnded{});
    }
    neomcp::MemoryLink link() {
        std::lock_guard lock(mutex_);
        if (!link_) throw std::logic_error("the scripted peer is not attached");
        return *link_;
    }

    OnRequest on_request_;
    mutable std::mutex mutex_;
    std::optional<neomcp::MemoryLink> link_;
    std::vector<Seen> written_;
    std::vector<neomcp::OutgoingKind> kinds_;
    std::vector<std::thread::id> threads_;
    std::size_t restarts_ = 0;
    bool detached_ = false;
};

// A handshake result at `revision`.
inline json initialize_result(std::string_view revision) {
    return json{{"protocolVersion", std::string(revision)},
                {"capabilities", json::object()},
                {"serverInfo", json{{"name", "scripted"}, {"version", "1"}}}};
}

// A discovery result that names `revision`.
inline json discover_result(std::string_view revision) {
    json list = json::array();
    list.push_back(std::string(revision));
    return json{{"supportedVersions", list}, {"capabilities", json::object()}, {"ttlMs", 0},
                {"cacheScope", "private"}, {"resultType", "complete"}};
}

// Releases waiting threads when it goes, so that a failed assertion never leaves a handler blocked.
class Latch {
public:
    Latch() = default;
    Latch(const Latch&) = delete;
    Latch& operator=(const Latch&) = delete;
    ~Latch() { open(); }

    void open() {
        {
            std::lock_guard lock(mutex_);
            open_ = true;
        }
        changed_.notify_all();
    }

    // Waits until the latch opens or `stop` is requested; whether it opened.
    bool wait(const std::stop_token& stop) {
        // The callback is made before the lock is taken and goes after it is let go, so that it never waits for it.
        std::stop_callback wake(stop, [this] {
            std::lock_guard inner(mutex_);
            changed_.notify_all();
        });
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, wait_bound, [&] { return open_ || stop.stop_requested(); });
        return open_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool open_ = false;
};

}  // namespace client_test
