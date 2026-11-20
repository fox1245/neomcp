// The client and its driver (neomcp/client.h).
//
// One thread per client runs an asio::io_context; the engine, the input dispatcher, the transport's events and the
// timers are touched there only. What the parts own:
// - Runtime: the io_context, the thread that runs it and the work guard that keeps it running. The thread holds the
//   runtime, so the io_context outlives every handler it runs, whoever lets the client's state go last.
// - ClientCore: the protocol state. It is shared by the Client handle and by every operation in flight (each
//   coroutine that runs on the client's thread holds it by value), so an operation never outlives it.
// The work guard is let go only once the client is closed and no admitted operation runs, so every operation that was
// posted to the client's thread runs there; one admitted after that is refused at once, with `closed`.
//
// An operation of an awaiter on another executor is a coroutine of that awaiter (a "parent") that hops to the client's
// thread with co_spawn and use_awaitable, so that it resumes on its own executor; asio carries the cancellation of the
// parent over to the child. Children tell their cancellation from their completion by state, never by an error code.
#include "neomcp/client.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/cancellation_state.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include "neomcp/wire_read.h"

namespace neomcp {

namespace detail {

namespace {

ClientError error_of(ClientErrorKind kind, std::string message) {
    ClientError error;
    error.kind = kind;
    error.message = std::move(message);
    return error;
}

ClientError closed_error() { return error_of(ClientErrorKind::closed, "the client is closed"); }

ClientError own_thread_error() {
    return error_of(ClientErrorKind::on_client_thread,
                    "a blocking method of the client was called on the client's own thread; use its async_ form");
}

// The failures after which what the server did is not known, when the request had gone out.
bool unknown_outcome(CallFailureKind kind) noexcept {
    return kind == CallFailureKind::timed_out || kind == CallFailureKind::cancelled ||
           kind == CallFailureKind::server_exited || kind == CallFailureKind::stream_lost;
}

// The failures whose answer was lost, after which a read may be sent again.
bool lost_answer(CallFailureKind kind) noexcept {
    return kind == CallFailureKind::server_exited || kind == CallFailureKind::stream_lost;
}

ClientError contact_error(const ContactFailure& failure) {
    ClientError error = error_of(error_kind(failure.kind), failure.message);
    error.server_revisions = failure.server_revisions;
    return error;
}

// A call's failure as the application sees it.
ClientError call_error(const CallFailure& failure, bool sent, const ContactState& contact) {
    ClientError error;
    if (const auto kind = error_kind(failure.kind)) {
        error = error_of(*kind, failure.message);
    } else if (contact.failure) {
        error = contact_error(*contact.failure);
    } else {
        error = error_of(ClientErrorKind::transport, failure.message);
    }
    error.may_have_run = sent && unknown_outcome(failure.kind);
    return error;
}

// Runs `operation` on a private io_context on the calling thread until it ends.
template <class T>
asio::awaitable<void> store_into(asio::awaitable<T> operation, std::optional<T>& out) {
    T value = co_await std::move(operation);
    out.emplace(std::move(value));
}

template <class T>
T block_on(asio::awaitable<T> operation) {
    asio::io_context local;
    std::optional<T> out;
    std::exception_ptr thrown;
    asio::co_spawn(local, store_into(std::move(operation), out),
                   [&thrown](std::exception_ptr error) { thrown = std::move(error); });
    local.run();
    if (thrown) std::rethrow_exception(thrown);
    if (!out) throw std::logic_error("neomcp: a client operation did not complete");
    return std::move(*out);
}

inline void block_on(asio::awaitable<void> operation) {
    asio::io_context local;
    std::exception_ptr thrown;
    asio::co_spawn(local, std::move(operation), [&thrown](std::exception_ptr error) { thrown = std::move(error); });
    local.run();
    if (thrown) std::rethrow_exception(thrown);
}

// ---------------------------------------------------------------------------------------------------------------
// The thread

class Runtime {
public:
    Runtime() : guard_(asio::make_work_guard(io_)) {}
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    ~Runtime() {
        if (!thread_.joinable()) return;
        // The last owner may be the thread itself, which then ends on its own.
        if (thread_.get_id() == std::this_thread::get_id()) {
            thread_.detach();
        } else {
            thread_.join();
        }
    }

    static void start(const std::shared_ptr<Runtime>& runtime) {
        runtime->thread_ = std::thread([held = runtime] { held->io_.run(); });
    }

    asio::io_context& io() noexcept { return io_; }
    asio::any_io_executor executor() noexcept { return io_.get_executor(); }
    bool here() noexcept { return io_.get_executor().running_in_this_thread(); }

    // Lets the io_context run out of work, on the client's thread.
    void release() {
        asio::post(io_, [this] { guard_.reset(); });
    }

    // Waits for the thread to end; not on the thread itself.
    void join() {
        if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
    }

private:
    asio::io_context io_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> guard_;
    std::thread thread_;
};

// Posts `handler` to the client's thread from any thread, while the runtime (and so its io_context) still exists;
// nothing once it is gone. A worker, a timer service or a stop callback that runs after the client's thread ended
// therefore never reaches a destroyed context. The runtime is held only for the post itself, never by the handler: a
// handler left in the queue of a context that no longer runs must not keep that context alive.
template <class Handler>
void post_to(const std::weak_ptr<Runtime>& runtime, Handler&& handler) {
    if (const std::shared_ptr<Runtime> held = runtime.lock()) asio::post(held->io(), std::forward<Handler>(handler));
}

// ---------------------------------------------------------------------------------------------------------------
// The default timers: steady timers on the client's thread.

class AsioTimers final : public TimerService {
public:
    explicit AsioTimers(asio::any_io_executor executor)
        : executor_(std::move(executor)), state_(std::make_shared<State>()) {}

    std::chrono::steady_clock::time_point now() const override { return std::chrono::steady_clock::now(); }

    TimerId start(std::chrono::milliseconds after, TimerPurpose, std::function<void()> fire) override {
        const TimerId id{state_->next++};
        auto timer = std::make_shared<asio::steady_timer>(executor_, after);
        state_->timers.emplace(id, timer);
        timer->async_wait([state = state_, id, fire = std::move(fire)](const std::error_code& error) {
            state->timers.erase(id);
            if (!error) fire();
        });
        return id;
    }

    void cancel(TimerId timer) override {
        const auto found = state_->timers.find(timer);
        if (found == state_->timers.end()) return;
        found->second->cancel();
        state_->timers.erase(found);
    }

private:
    struct State {
        std::uint64_t next = 1;
        std::map<TimerId, std::shared_ptr<asio::steady_timer>> timers;
    };
    asio::any_io_executor executor_;
    std::shared_ptr<State> state_;
};

// ---------------------------------------------------------------------------------------------------------------
// The worker pool of plain input handlers

class WorkerPool {
public:
    using Done = std::function<void(InputJobId, HandlerOutcome)>;

    WorkerPool(std::size_t most, Done done) : state_(std::make_shared<State>()) {
        state_->most = most;
        state_->done = std::move(done);
    }
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    ~WorkerPool() {
        stop();
        // Workers hold their own state: one still running a handler ends when the handler returns.
        for (auto& thread : threads_) {
            if (thread.joinable()) thread.detach();
        }
    }

    // On the client's thread.
    void push(InputJob job) {
        std::lock_guard lock(state_->mutex);
        if (state_->stopping) return;
        state_->queue.push_back(std::move(job));
        if (state_->queue.size() > state_->idle && state_->threads < state_->most) {
            ++state_->threads;
            threads_.emplace_back(work, state_);
        }
        state_->ready.notify_one();
    }

    // Jobs not yet started are dropped; running ones finish (their stop was requested).
    void stop() {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
        state_->queue.clear();
        state_->ready.notify_all();
    }

    // Waits for every worker to end; not on a worker.
    void join() {
        for (auto& thread : threads_) {
            if (thread.joinable() && thread.get_id() != std::this_thread::get_id()) thread.join();
        }
    }

private:
    struct State {
        std::mutex mutex;
        std::condition_variable ready;
        std::deque<InputJob> queue;
        std::size_t most = 1;
        std::size_t threads = 0;
        std::size_t idle = 0;
        bool stopping = false;
        Done done;
    };

    static void work(std::shared_ptr<State> state) {
        while (true) {
            InputJob job;
            {
                std::unique_lock lock(state->mutex);
                ++state->idle;
                state->ready.wait(lock, [&] { return state->stopping || !state->queue.empty(); });
                --state->idle;
                if (state->stopping) return;
                job = std::move(state->queue.front());
                state->queue.pop_front();
            }
            // A job whose outcome is no longer awaited is dropped before it starts.
            if (job.stop.stop_requested()) continue;
            HandlerOutcome outcome = job.plain();
            state->done(job.id, std::move(outcome));
        }
    }

    std::shared_ptr<State> state_;
    std::vector<std::thread> threads_;
};

// What an operation on the client's thread gives back to its parent. Default-constructible, as co_spawn needs.
struct CallOutcome {
    std::optional<json> result;
    profile::ProfilePtr profile;
    std::optional<ClientError> error;
};

struct OpenOutcome {
    std::optional<ClientError> error;
};

struct Done {};

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// The client's state

class ClientCore final : public std::enable_shared_from_this<ClientCore>, public TransportEvents, public InputRunner {
public:
    ClientCore(ClientOptions options, std::unique_ptr<Transport> transport, std::shared_ptr<Runtime> runtime)
        : runtime_(std::move(runtime)),
          options_(std::move(options)),
          transport_(std::move(transport)),
          contact_signal_(runtime_->io()),
          close_signal_(runtime_->io()),
          transport_signal_(runtime_->io()) {
        for (asio::steady_timer* waiting : {&contact_signal_, &close_signal_, &transport_signal_}) {
            waiting->expires_at(asio::steady_timer::time_point::max());
        }
        timers_ = options_.timers ? options_.timers : std::make_shared<AsioTimers>(runtime_->executor());
    }

    ClientCore(const ClientCore&) = delete;
    ClientCore& operator=(const ClientCore&) = delete;

    ~ClientCore() override {
        // A client is closed before its state is let go; this only keeps a state that was never opened from holding
        // its thread.
        std::lock_guard lock(admission_);
        if (!released_) {
            released_ = true;
            runtime_->release();
        }
    }

    // Checks the options and builds the engine and the dispatcher. Nothing runs yet.
    static std::variant<std::shared_ptr<ClientCore>, ClientError> create(ClientOptions options,
                                                                        std::unique_ptr<Transport> transport) {
        if (!transport) return error_of(ClientErrorKind::invalid_options, "the client needs a transport");
        std::string problems;
        if (options.max_concurrent_input_handlers == 0) problems += "max_concurrent_input_handlers must be positive\n";
        if (options.state_retry_first_delay.count() <= 0 || options.state_retry_max_delay.count() <= 0) {
            problems += "the delays between retries of a call that asks for its state alone must be positive\n";
        } else if (options.state_retry_first_delay > options.state_retry_max_delay) {
            problems += "state_retry_first_delay must not exceed state_retry_max_delay\n";
        }
        if (!problems.empty()) {
            problems.pop_back();
            return error_of(ClientErrorKind::invalid_options, std::move(problems));
        }
        auto runtime = std::make_shared<Runtime>();
        auto core = std::make_shared<ClientCore>(std::move(options), std::move(transport), runtime);
        if (auto problem = core->build()) return std::move(*problem);
        Runtime::start(runtime);
        return core;
    }

    asio::any_io_executor executor() const noexcept override { return runtime_->executor(); }
    asio::io_context& io() noexcept { return runtime_->io(); }
    bool here() const noexcept { return runtime_->here(); }

    // ---- Admission: what may still be posted to the client's thread

    // An operation of the application; refused once the client began to close.
    bool admit() {
        std::lock_guard lock(admission_);
        if (!accepting_) return false;
        ++in_flight_;
        return true;
    }

    // An operation that closes; refused only once the thread is let go.
    bool admit_close() {
        std::lock_guard lock(admission_);
        if (released_) return false;
        ++in_flight_;
        return true;
    }

    // On the client's thread, when an admitted operation ends there.
    void leave() {
        std::lock_guard lock(admission_);
        --in_flight_;
        release_if_done();
    }

    // ---- What the application reads from any thread

    ServerDescription description() const {
        std::lock_guard lock(info_);
        return description_;
    }

    std::optional<ContactVerdict> verdict() const {
        std::lock_guard lock(info_);
        return verdict_;
    }

    // ---- Operations, on the client's thread

    // Starts the transport and contact; empty when contact is under way.
    std::optional<ClientError> start_contact() {
        if (auto problem = transport_->start(*this)) {
            return error_of(ClientErrorKind::transport, *problem);
        }
        transport_started_ = true;
        engine_->open();
        pump();
        return std::nullopt;
    }

    bool contact_settled() const {
        const ContactPhase phase = engine_->contact().phase;
        return phase == ContactPhase::open || phase == ContactPhase::failed || phase == ContactPhase::closed;
    }

    asio::steady_timer& contact_signal() noexcept { return contact_signal_; }

    // What opening came to.
    std::optional<ClientError> contact_result() const {
        const ContactState& contact = engine_->contact();
        if (contact.phase == ContactPhase::open) return std::nullopt;
        if (contact.phase == ContactPhase::failed && contact.failure) return contact_error(*contact.failure);
        if (contact.phase == ContactPhase::closed) return closed_error();
        return error_of(ClientErrorKind::cancelled, "opening the client was cancelled");
    }

    // One call: what begin_call made of it, and the waiter that its end wakes.
    struct Waiter {
        explicit Waiter(asio::io_context& context) : wake(context) {
            wake.expires_at(asio::steady_timer::time_point::max());
        }
        RequestRef ref;
        OutgoingCall call;  // kept for sending it again
        bool resent = false;
        std::function<void(const Progress&)> on_progress;
        std::optional<RequestEnded> ended;
        asio::steady_timer wake;
        std::optional<std::stop_callback<std::function<void()>>> stop;
    };

    // Sends a call, or says why it cannot go out.
    std::optional<ClientError> begin_call(const std::shared_ptr<Waiter>& waiter, ClientMessage message,
                                          RequestOptions options, json input_schema) {
        if (closing_) return closed_error();
        if (options.timeout && options.timeout->count() <= 0) {
            return error_of(ClientErrorKind::invalid_options, "the timeout of a call must be positive");
        }
        if (options.stop.stop_requested()) {
            return error_of(ClientErrorKind::cancelled, "the call was cancelled before it was sent");
        }
        OutgoingCall call;
        call.message = std::move(message);
        call.timeout = options.timeout;
        call.reset_timeout_on_progress = options.reset_timeout_on_progress;
        call.progress = static_cast<bool>(options.on_progress);
        call.input_schema = std::move(input_schema);
        waiter->call = call;
        waiter->on_progress = std::move(options.on_progress);
        auto sent = engine_->send(std::move(call));
        if (auto* failure = std::get_if<CallFailure>(&sent)) {
            ClientError error = call_error(*failure, false, engine_->contact());
            pump();
            return error;
        }
        waiter->ref = std::get<RequestRef>(sent);
        waiters_.emplace(waiter->ref, waiter);
        if (options.stop.stop_possible()) {
            std::function<void()> on_stop = [weak = weak_from_this(), held = std::weak_ptr<Waiter>(waiter),
                                             runtime = std::weak_ptr<Runtime>(runtime_)] {
                post_to(runtime, [weak, held] {
                    const std::shared_ptr<ClientCore> core = weak.lock();
                    const std::shared_ptr<Waiter> one = held.lock();
                    if (core && one) core->cancel_call(*one);
                });
            };
            waiter->stop.emplace(options.stop, std::move(on_stop));
        }
        pump();
        return std::nullopt;
    }

    // The application cancels a call that has not ended.
    void cancel_call(Waiter& waiter) {
        if (waiter.ended) return;
        engine_->cancel(waiter.ref, "The client cancelled the request");
        pump();
        if (waiter.ended) return;
        // The engine no longer knows the call: it ended without the event reaching the waiter.
        waiters_.erase(waiter.ref);
        waiter.ended = RequestEnded{waiter.ref, CallFailure{CallFailureKind::cancelled, "the call was cancelled"},
                                    nullptr};
        waiter.wake.cancel();
    }

    CallOutcome finish_call(Waiter& waiter, std::optional<ClientError> refused) {
        waiter.stop.reset();
        CallOutcome outcome;
        if (refused) {
            outcome.error = std::move(refused);
            return outcome;
        }
        if (!waiter.ended) {
            outcome.error = closed_error();
            return outcome;
        }
        RequestEnded& ended = *waiter.ended;
        if (auto* answered = std::get_if<Answered>(&ended.outcome)) {
            outcome.result = std::move(answered->result);
            outcome.profile = ended.profile;
        } else if (auto* refusal = std::get_if<ErrorAnswered>(&ended.outcome)) {
            ClientError error = error_of(ClientErrorKind::server_error, std::move(refusal->error.message));
            error.code = refusal->error.code;
            error.data = std::move(refusal->error.data);
            outcome.error = std::move(error);
        } else {
            outcome.error = call_error(std::get<CallFailure>(ended.outcome), ended.profile != nullptr,
                                       engine_->contact());
        }
        return outcome;
    }

    bool roots_changed_here() {
        if (closing_ || !engine_) return false;
        const bool sent = engine_->roots_changed();
        pump();
        return sent;
    }

    // Closes everything, once; a second closer waits for the first.
    bool begin_close() {
        if (closing_) return false;
        closing_ = true;
        {
            std::lock_guard lock(admission_);
            accepting_ = false;
        }
        for (auto& [ref, retry] : state_retries_) {
            if (retry.timer) timers_->cancel(*retry.timer);
        }
        state_retries_.clear();
        dispatcher_->close();
        pool_->stop();
        engine_->close();
        pump();
        for (auto& [key, clock] : clocks_) {
            if (clock.timer) timers_->cancel(*clock.timer);
        }
        clocks_.clear();
        return true;
    }

    bool closed() const noexcept { return closed_; }
    asio::steady_timer& close_signal() noexcept { return close_signal_; }
    asio::steady_timer& transport_signal() noexcept { return transport_signal_; }

    // Whether this caller closes the transport (no one began yet); else it waits for transport_signal().
    bool claim_transport_close() {
        if (transport_closing_) return false;
        transport_closing_ = true;
        return true;
    }
    bool transport_closed() const noexcept { return transport_closed_ || !transport_started_; }
    asio::awaitable<void> close_transport(CloseCause cause) { return transport_->close(cause); }
    void transport_did_close() {
        transport_closed_ = true;
        transport_signal_.cancel();
    }

    void finish_close() {
        closed_ = true;
        close_signal_.cancel();
        std::lock_guard lock(admission_);
        close_finished_ = true;
        release_if_done();
    }

    // Waits for the plain handlers still running; not on the client's thread.
    void join_workers() {
        if (pool_) pool_->join();
    }
    void join_thread() { runtime_->join(); }

    // ---- TransportEvents

    Reception received(UnitId unit, std::string_view message) override {
        if (closing_) return Reception::ignored;
        const Reception reception = engine_->received(unit, message);
        pump();
        return reception;
    }

    void stream_event(UnitId unit, StreamEvent event) override {
        if (closing_) return;
        engine_->stream_event(unit, std::move(event));
        pump();
    }

    void restarted() override {
        if (closing_) return;
        if (opened_) {
            ++restarts_;
            restart_unreported_ = true;
        }
        engine_->transport_restarted();
        pump();
    }

    // ---- InputRunner

    void start(InputJob job) override {
        if (job.coroutine) {
            spawn_handler(std::move(job));
        } else {
            pool_->push(std::move(job));
        }
    }

private:
    // ---- Setup

    std::optional<ClientError> build() {
        EngineOptions engine;
        engine.profiles = options_.profiles;
        engine.client_info = options_.client_info;
        engine.log_level = options_.log_level;
        engine.input = options_.handlers.support();
        engine.prior = options_.prior;
        engine.mechanisms = options_.mechanisms;
        engine.request_timeout = options_.request_timeout;
        engine.max_total_timeout = options_.max_total_timeout;
        engine.discover_probe_timeout = options_.discover_probe_timeout;
        engine.input_handler_timeout = options_.input_handler_timeout;
        engine.max_pending_requests = options_.max_pending_requests;
        engine.max_queued_events = options_.max_queued_events;
        engine.max_message_bytes = options_.max_message_bytes;
        engine.max_nesting_depth = options_.max_nesting_depth;
        engine.max_input_rounds = options_.max_input_rounds;
        engine.max_input_requests_per_round = options_.max_input_requests_per_round;
        engine.max_concurrent_server_requests = options_.max_concurrent_server_requests;
        engine.max_restarts = options_.max_restarts;
        engine.peer_schema_depth = options_.peer_schema_depth;
        engine.peer_pattern_length = options_.peer_pattern_length;
        engine.peer_pattern_memory = options_.peer_pattern_memory;
        engine.peer_max_evaluations = options_.peer_max_evaluations;
        auto made = ClientEngine::create(std::move(engine), transport_->kind(),
                                         [this](Outgoing outgoing) { outbox_.push_back(std::move(outgoing)); });
        if (auto* error = std::get_if<ClientMechanismsError>(&made)) {
            return error_of(ClientErrorKind::invalid_options, error->message);
        }
        engine_ = std::move(std::get<std::unique_ptr<ClientEngine>>(made));
        // A worker hands its outcome back from its own thread, possibly after the client's thread ended (a client
        // destroyed on its own thread does not wait for its handlers).
        WorkerPool::Done done = [weak = weak_from_this(), runtime = std::weak_ptr<Runtime>(runtime_)](
                                    InputJobId id, HandlerOutcome outcome) {
            post_to(runtime, [weak, id, outcome = std::move(outcome)]() mutable {
                const std::shared_ptr<ClientCore> core = weak.lock();
                if (core) core->complete_job(id, std::move(outcome));
            });
        };
        pool_ = std::make_unique<WorkerPool>(options_.max_concurrent_input_handlers, std::move(done));
        dispatcher_ = std::make_unique<InputDispatcher>(*engine_, options_.handlers, *this);
        return std::nullopt;
    }

    // Under admission_.
    void release_if_done() {
        if (released_ || accepting_ || in_flight_ != 0 || !close_finished_) return;
        released_ = true;
        runtime_->release();
    }

    // ---- The pump: what the engine wrote goes to the transport, and its events to their places.

    void pump() {
        if (pumping_) {
            pump_again_ = true;
            return;
        }
        pumping_ = true;
        do {
            pump_again_ = false;
            while (!outbox_.empty()) {
                std::vector<Outgoing> batch;
                batch.swap(outbox_);
                for (Outgoing& outgoing : batch) {
                    if (outgoing.kind == OutgoingKind::close) {
                        engine_wrote_close_ = true;
                        continue;
                    }
                    if (!transport_closing_) transport_->send(std::move(outgoing));
                }
            }
            while (auto event = engine_->next_event()) handle(std::move(*event));
            while (auto report = dispatcher_->next_report()) diagnose(ClientDiagnostic(std::move(*report)));
        } while (pump_again_ || !outbox_.empty());
        pumping_ = false;
        // Contact cannot go on (a handshake that was never answered): the transport ends at once.
        if (engine_wrote_close_ && !closing_ && !transport_closing_ && transport_started_) {
            transport_closing_ = true;
            asio::co_spawn(runtime_->io(), end_transport(shared_from_this()), asio::detached);
        }
    }

    static asio::awaitable<void> end_transport(std::shared_ptr<ClientCore> self) {
        co_await self->close_transport(CloseCause::contact_ended);
        self->transport_did_close();
    }

    void handle(EngineEvent event) {
        if (auto* change = std::get_if<DeadlineChange>(&event)) {
            on_deadline(*change);
        } else if (auto* ended = std::get_if<RequestEnded>(&event)) {
            dispatcher_->take(event);
            end_state_retry(ended->request);
            on_ended(std::move(*ended));
        } else if (auto* started = std::get_if<InputRoundStarted>(&event)) {
            if (started->asking.requests.empty()) {
                state_only_round(started->request);
            } else {
                end_state_retry(started->request);
                dispatcher_->take(event);
            }
        } else if (std::holds_alternative<ServerAsked>(event) || std::holds_alternative<ServerAskEnded>(event)) {
            dispatcher_->take(event);
        } else if (auto* progress = std::get_if<ProgressEvent>(&event)) {
            on_progress(*progress);
        } else if (auto* logged = std::get_if<LogEvent>(&event)) {
            if (options_.on_log) guarded([&] { options_.on_log(*logged); });
        } else if (auto* contact = std::get_if<ContactChanged>(&event)) {
            on_contact(contact->phase);
        } else if (auto* diagnostic = std::get_if<Diagnostic>(&event)) {
            diagnose(ClientDiagnostic(std::move(*diagnostic)));
        }
        // A change feed's messages have no listener in this client: they are dropped.
    }

    // A callback must not throw into the client's thread: what it throws is dropped.
    template <class F>
    static void guarded(F&& callback) noexcept {
        try {
            callback();
        } catch (...) {
        }
    }

    void diagnose(const ClientDiagnostic& diagnostic) {
        if (options_.on_diagnostic) guarded([&] { options_.on_diagnostic(diagnostic); });
    }

    void on_progress(const ProgressEvent& event) {
        const auto found = waiters_.find(event.request);
        if (found == waiters_.end() || !found->second->on_progress) return;
        const Progress progress{event.progress, event.total, event.message};
        const auto& callback = found->second->on_progress;
        guarded([&] { callback(progress); });
    }

    void on_contact(ContactPhase phase) {
        if (phase == ContactPhase::open) {
            update_contact_info();
            if (opened_ && restart_unreported_) {
                restart_unreported_ = false;
                const ContactState& contact = engine_->contact();
                ServerRestarted notice;
                notice.restarts = restarts_;
                notice.handshake_repeated = contact.verdict && contact.verdict->kind == ServerKind::handshake;
                diagnose(ClientDiagnostic(notice));
            }
            opened_ = true;
        }
        if (phase == ContactPhase::open || phase == ContactPhase::failed || phase == ContactPhase::closed) {
            contact_signal_.cancel();
        }
    }

    void update_contact_info() {
        const ContactState& contact = engine_->contact();
        ServerDescription seen;
        if (contact.verdict) {
            seen.revision = contact.verdict->revision;
            seen.kind = contact.verdict->kind;
            seen.transport = contact.verdict->transport;
        }
        seen.info_wire = contact.server.info;
        if (contact.profile && contact.server.info.is_object()) {
            auto read = from_wire<Implementation>(contact.server.info, *contact.profile);
            if (auto* info = std::get_if<Implementation>(&read)) seen.info = std::move(*info);
        }
        seen.capabilities = contact.server.capabilities;
        seen.instructions = contact.server.instructions;
        seen.revisions = contact.revisions;
        std::lock_guard lock(info_);
        description_ = std::move(seen);
        verdict_ = contact.verdict;
    }

    void on_ended(RequestEnded ended) {
        const auto found = waiters_.find(ended.request);
        if (found == waiters_.end()) return;
        const std::shared_ptr<Waiter> waiter = found->second;
        waiters_.erase(found);
        if (const auto* failure = std::get_if<CallFailure>(&ended.outcome);
            failure != nullptr && lost_answer(failure->kind) && !waiter->resent && !closing_ &&
            options_.resent_methods.count(waiter->call.message.method) != 0) {
            // The answer of a read was lost: it is asked for once more, under a new request and a clock of its own.
            waiter->resent = true;
            auto sent = engine_->send(waiter->call);
            if (auto* ref = std::get_if<RequestRef>(&sent)) {
                waiter->ref = *ref;
                waiters_.emplace(*ref, waiter);
                return;
            }
        }
        waiter->ended = std::move(ended);
        waiter->wake.cancel();
    }

    // ---- Clocks

    struct ClockTimer {
        std::optional<TimerId> timer;
        std::chrono::milliseconds left{0};
        std::chrono::steady_clock::time_point since;
    };

    void on_deadline(const DeadlineChange& change) {
        const auto key = std::make_pair(change.deadline.owner, change.deadline.kind);
        switch (change.action) {
            case DeadlineAction::start: {
                ClockTimer& clock = clocks_[key];
                disarm(clock);
                clock.left = change.duration;
                clock.since = timers_->now();
                clock.timer = arm(change.deadline, clock.left);
                return;
            }
            case DeadlineAction::pause: {
                const auto found = clocks_.find(key);
                if (found == clocks_.end()) return;
                ClockTimer& clock = found->second;
                disarm(clock);
                const auto ran = std::chrono::duration_cast<std::chrono::milliseconds>(timers_->now() - clock.since);
                clock.left = std::max(std::chrono::milliseconds{0}, clock.left - ran);
                return;
            }
            case DeadlineAction::resume: {
                const auto found = clocks_.find(key);
                if (found == clocks_.end()) return;
                ClockTimer& clock = found->second;
                disarm(clock);
                clock.since = timers_->now();
                clock.timer = arm(change.deadline, clock.left);
                return;
            }
            case DeadlineAction::stop: {
                const auto found = clocks_.find(key);
                if (found == clocks_.end()) return;
                disarm(found->second);
                clocks_.erase(found);
                return;
            }
        }
    }

    TimerId arm(const Deadline& deadline, std::chrono::milliseconds after) {
        std::function<void()> fire = [weak = weak_from_this(), runtime = std::weak_ptr<Runtime>(runtime_), deadline] {
            post_to(runtime, [weak, deadline] {
                const std::shared_ptr<ClientCore> core = weak.lock();
                if (core) core->deadline_lapsed(deadline);
            });
        };
        return timers_->start(after, TimerPurpose(deadline), std::move(fire));
    }

    void disarm(ClockTimer& clock) {
        if (clock.timer) timers_->cancel(*clock.timer);
        clock.timer.reset();
    }

    void deadline_lapsed(const Deadline& deadline) {
        if (closing_) return;
        engine_->expire(deadline);
        pump();
    }

    // ---- A round that asks for nothing but its state

    struct StateRetry {
        std::size_t count = 0;
        std::optional<TimerId> timer;
    };

    void state_only_round(RequestRef call) {
        StateRetry& retry = state_retries_[call];
        ++retry.count;
        if (retry.count == 1) {
            engine_->answer_round(call, {});
            return;
        }
        std::chrono::milliseconds delay = options_.state_retry_first_delay;
        for (std::size_t step = 2; step < retry.count; ++step) {
            if (delay >= options_.state_retry_max_delay) break;
            delay *= 2;
        }
        delay = std::min(delay, options_.state_retry_max_delay);
        const std::size_t count = retry.count;
        std::function<void()> fire = [weak = weak_from_this(), runtime = std::weak_ptr<Runtime>(runtime_), call,
                                      count] {
            post_to(runtime, [weak, call, count] {
                const std::shared_ptr<ClientCore> core = weak.lock();
                if (core) core->state_retry_due(call, count);
            });
        };
        retry.timer = timers_->start(delay, TimerPurpose(StateRetryDelay{call, count}), std::move(fire));
    }

    void state_retry_due(RequestRef call, std::size_t count) {
        if (closing_) return;
        const auto found = state_retries_.find(call);
        if (found == state_retries_.end() || found->second.count != count || !found->second.timer) return;
        found->second.timer.reset();
        engine_->answer_round(call, {});
        pump();
    }

    void end_state_retry(RequestRef call) {
        const auto found = state_retries_.find(call);
        if (found == state_retries_.end()) return;
        if (found->second.timer) timers_->cancel(*found->second.timer);
        state_retries_.erase(found);
    }

    // ---- Coroutine input handlers, on the client's thread; stop cancels what the handler awaits.

    void spawn_handler(InputJob job) {
        auto cancel_signal = std::make_shared<asio::cancellation_signal>();
        auto stop = std::make_shared<std::optional<std::stop_callback<std::function<void()>>>>();
        std::function<void()> on_stop = [runtime = std::weak_ptr<Runtime>(runtime_),
                                         held = std::weak_ptr<asio::cancellation_signal>(cancel_signal)] {
            post_to(runtime, [held] {
                const std::shared_ptr<asio::cancellation_signal> one = held.lock();
                if (one) one->emit(asio::cancellation_type::terminal);
            });
        };
        stop->emplace(job.stop, std::move(on_stop));
        const InputJobId id = job.id;
        asio::co_spawn(runtime_->io(), job.coroutine(),
                       asio::bind_cancellation_slot(
                           cancel_signal->slot(), [weak = weak_from_this(), id, cancel_signal, stop](
                                                      std::exception_ptr, HandlerOutcome outcome) {
                               stop->reset();
                               const std::shared_ptr<ClientCore> core = weak.lock();
                               if (core) core->complete_job(id, std::move(outcome));
                           }));
    }

    void complete_job(InputJobId id, HandlerOutcome outcome) {
        if (closing_) return;
        dispatcher_->complete(id, std::move(outcome));
        pump();
    }

    // ---- State

    std::shared_ptr<Runtime> runtime_;
    ClientOptions options_;
    std::shared_ptr<TimerService> timers_;
    std::unique_ptr<ClientEngine> engine_;
    std::unique_ptr<WorkerPool> pool_;
    std::unique_ptr<InputDispatcher> dispatcher_;
    std::unique_ptr<Transport> transport_;
    asio::steady_timer contact_signal_;
    asio::steady_timer close_signal_;
    asio::steady_timer transport_signal_;

    std::vector<Outgoing> outbox_;
    std::map<RequestRef, std::shared_ptr<Waiter>> waiters_;
    std::map<std::pair<DeadlineOwner, DeadlineKind>, ClockTimer> clocks_;
    std::map<RequestRef, StateRetry> state_retries_;
    bool pumping_ = false;
    bool pump_again_ = false;
    bool engine_wrote_close_ = false;
    bool transport_started_ = false;
    bool transport_closing_ = false;
    bool transport_closed_ = false;
    bool closing_ = false;
    bool closed_ = false;
    bool opened_ = false;
    bool restart_unreported_ = false;
    std::size_t restarts_ = 0;

    mutable std::mutex info_;
    ServerDescription description_;
    std::optional<ContactVerdict> verdict_;

    std::mutex admission_;
    bool accepting_ = true;
    bool close_finished_ = false;
    bool released_ = false;
    std::size_t in_flight_ = 0;
};

namespace {

// Ends an admitted operation on the client's thread when its coroutine frame goes.
class Admitted {
public:
    explicit Admitted(std::shared_ptr<ClientCore> core) : core_(std::move(core)) {}
    Admitted(const Admitted&) = delete;
    Admitted& operator=(const Admitted&) = delete;
    ~Admitted() { core_->leave(); }

private:
    std::shared_ptr<ClientCore> core_;
};

// ---- Children: coroutines on the client's thread. Each holds the state by value.

asio::awaitable<CallOutcome> run_call(std::shared_ptr<ClientCore> core, ClientMessage message,
                                      RequestOptions options, json input_schema) {
    const Admitted admitted(core);
    co_await asio::this_coro::throw_if_cancelled(false);
    auto waiter = std::make_shared<ClientCore::Waiter>(core->io());
    std::optional<ClientError> refused =
        core->begin_call(waiter, std::move(message), std::move(options), std::move(input_schema));
    if (!refused && !waiter->ended) {
        const asio::cancellation_state state = co_await asio::this_coro::cancellation_state;
        if (state.cancelled() == asio::cancellation_type::none) {
            std::error_code woken;
            co_await waiter->wake.async_wait(asio::redirect_error(asio::use_awaitable, woken));
        }
        // Woken without an end: the awaiter was cancelled.
        if (!waiter->ended) core->cancel_call(*waiter);
    }
    CallOutcome outcome = core->finish_call(*waiter, std::move(refused));
    co_return outcome;
}

asio::awaitable<OpenOutcome> run_open(std::shared_ptr<ClientCore> core) {
    const Admitted admitted(core);
    co_await asio::this_coro::throw_if_cancelled(false);
    OpenOutcome outcome;
    outcome.error = core->start_contact();
    if (!outcome.error && !core->contact_settled()) {
        std::error_code woken;
        co_await core->contact_signal().async_wait(asio::redirect_error(asio::use_awaitable, woken));
    }
    if (!outcome.error) outcome.error = core->contact_result();
    co_return outcome;
}

asio::awaitable<Done> run_close(std::shared_ptr<ClientCore> core) {
    const Admitted admitted(core);
    co_await asio::this_coro::throw_if_cancelled(false);
    Done done;
    if (core->closed()) co_return done;
    if (!core->begin_close()) {
        std::error_code woken;
        while (!core->closed()) {
            co_await core->close_signal().async_wait(asio::redirect_error(asio::use_awaitable, woken));
        }
        co_return done;
    }
    if (core->claim_transport_close()) {
        co_await core->close_transport(CloseCause::client_closed);
        core->transport_did_close();
    } else {
        std::error_code woken;
        while (!core->transport_closed()) {
            co_await core->transport_signal().async_wait(asio::redirect_error(asio::use_awaitable, woken));
        }
    }
    core->finish_close();
    co_return done;
}

asio::awaitable<bool> run_roots_changed(std::shared_ptr<ClientCore> core) {
    const Admitted admitted(core);
    const bool sent = core->roots_changed_here();
    co_return sent;
}

// ---- Parents: coroutines of the awaiter.

asio::awaitable<CallOutcome> call_on(std::shared_ptr<ClientCore> core, ClientMessage message, RequestOptions options,
                                     json input_schema) {
    CallOutcome outcome;
    if (!core || !core->admit()) {
        outcome.error = closed_error();
        co_return outcome;
    }
    outcome = co_await asio::co_spawn(
        core->executor(), run_call(core, std::move(message), std::move(options), std::move(input_schema)),
        asio::use_awaitable);
    co_return outcome;
}

asio::awaitable<void> close_on(std::shared_ptr<ClientCore> core) {
    co_await asio::this_coro::throw_if_cancelled(false);
    if (!core || !core->admit_close()) co_return;
    const Done done = co_await asio::co_spawn(core->executor(), run_close(core), asio::use_awaitable);
    static_cast<void>(done);
}

Result<json> json_result(CallOutcome outcome) {
    if (outcome.error) return std::move(*outcome.error);
    return std::move(*outcome.result);
}

Result<void> void_result(CallOutcome outcome) {
    if (outcome.error) return std::move(*outcome.error);
    return {};
}

Result<ToolResult> tool_result(CallOutcome outcome) {
    if (outcome.error) return std::move(*outcome.error);
    if (!outcome.profile) return error_of(ClientErrorKind::malformed_answer, "the tool's result has no revision");
    auto read = from_wire<ToolResult>(*outcome.result, *outcome.profile);
    if (auto* problem = std::get_if<std::string>(&read)) {
        return error_of(ClientErrorKind::malformed_answer, "the tool's result cannot be read: " + *problem);
    }
    return std::get<ToolResult>(std::move(read));
}

asio::awaitable<Result<json>> request_on(std::shared_ptr<ClientCore> core, std::string method, json params,
                                         RequestOptions options) {
    ClientMessage message{std::move(method), std::move(params)};
    CallOutcome outcome = co_await call_on(std::move(core), std::move(message), std::move(options), nullptr);
    Result<json> result = json_result(std::move(outcome));
    co_return result;
}

asio::awaitable<Result<void>> ping_on(std::shared_ptr<ClientCore> core, RequestOptions options) {
    ClientMessage message{std::string(client_method::ping), nullptr};
    CallOutcome outcome = co_await call_on(std::move(core), std::move(message), std::move(options), nullptr);
    Result<void> result = void_result(std::move(outcome));
    co_return result;
}

constexpr std::string_view name_member = "name";
constexpr std::string_view arguments_member = "arguments";

asio::awaitable<Result<ToolResult>> call_tool_on(std::shared_ptr<ClientCore> core, std::string name, json arguments,
                                                 RequestOptions options) {
    json params = json::object();
    params[std::string(name_member)] = std::move(name);
    params[std::string(arguments_member)] = std::move(arguments);
    ClientMessage message{std::string(client_method::tools_call), std::move(params)};
    CallOutcome outcome = co_await call_on(std::move(core), std::move(message), std::move(options), nullptr);
    Result<ToolResult> result = tool_result(std::move(outcome));
    co_return result;
}

}  // namespace

}  // namespace detail

// ---------------------------------------------------------------------------------------------------------------
// Client

Client::Client(std::shared_ptr<detail::ClientCore> core) noexcept : core_(std::move(core)) {}

Client::Client(Client&& other) noexcept = default;

Client& Client::operator=(Client&& other) noexcept {
    if (this == &other) return *this;
    Client closing(std::move(*this));
    core_ = std::move(other.core_);
    return *this;
}

Client::~Client() {
    if (!core_) return;
    if (core_->here()) {
        // The client's own thread cannot wait for itself: the close goes on there, and the thread ends after it.
        if (core_->admit_close()) asio::co_spawn(core_->executor(), detail::run_close(core_), asio::detached);
        return;
    }
    try {
        detail::block_on(detail::close_on(core_));
    } catch (...) {
    }
    core_->join_workers();
    core_->join_thread();
}

asio::awaitable<Result<Client>> Client::async_open(ClientOptions options, std::unique_ptr<Transport> transport) {
    co_await asio::this_coro::throw_if_cancelled(false);
    auto created = detail::ClientCore::create(std::move(options), std::move(transport));
    if (auto* refused = std::get_if<ClientError>(&created)) {
        Result<Client> failed(std::move(*refused));
        co_return failed;
    }
    std::shared_ptr<detail::ClientCore> core = std::get<std::shared_ptr<detail::ClientCore>>(std::move(created));
    detail::OpenOutcome opened;
    if (core->admit()) {
        opened = co_await asio::co_spawn(core->executor(), detail::run_open(core), asio::use_awaitable);
    } else {
        opened.error = detail::closed_error();
    }
    if (opened.error) {
        co_await detail::close_on(core);
        core->join_workers();
        core->join_thread();
        Result<Client> failed(std::move(*opened.error));
        co_return failed;
    }
    Result<Client> client(Client(std::move(core)));
    co_return client;
}

Result<Client> Client::open(ClientOptions options, std::unique_ptr<Transport> transport) {
    return detail::block_on(async_open(std::move(options), std::move(transport)));
}

asio::awaitable<Result<Client>> Client::failed_open(ClientError error) {
    Result<Client> failed(std::move(error));
    co_return failed;
}

Result<void> Client::close() {
    if (core_ && core_->here()) return detail::own_thread_error();
    detail::block_on(async_close());
    return {};
}

asio::awaitable<void> Client::async_close() { return detail::close_on(core_); }

Result<json> Client::request(std::string method, json params, RequestOptions options) {
    if (core_ && core_->here()) return detail::own_thread_error();
    return detail::block_on(async_request(std::move(method), std::move(params), std::move(options)));
}

asio::awaitable<Result<json>> Client::async_request(std::string method, json params, RequestOptions options) {
    return detail::request_on(core_, std::move(method), std::move(params), std::move(options));
}

Result<void> Client::ping(RequestOptions options) {
    if (core_ && core_->here()) return detail::own_thread_error();
    return detail::block_on(async_ping(std::move(options)));
}

asio::awaitable<Result<void>> Client::async_ping(RequestOptions options) {
    return detail::ping_on(core_, std::move(options));
}

Result<ToolResult> Client::call_tool(std::string name, json arguments, RequestOptions options) {
    if (core_ && core_->here()) return detail::own_thread_error();
    return detail::block_on(async_call_tool(std::move(name), std::move(arguments), std::move(options)));
}

asio::awaitable<Result<ToolResult>> Client::async_call_tool(std::string name, json arguments, RequestOptions options) {
    return detail::call_tool_on(core_, std::move(name), std::move(arguments), std::move(options));
}

bool Client::roots_changed() {
    if (!core_) return false;
    if (core_->here()) return core_->roots_changed_here();
    if (!core_->admit()) return false;
    return detail::block_on(asio::co_spawn(core_->executor(), detail::run_roots_changed(core_), asio::use_awaitable));
}

ServerDescription Client::server() const { return core_ ? core_->description() : ServerDescription{}; }

std::optional<ContactVerdict> Client::verdict() const { return core_ ? core_->verdict() : std::nullopt; }

}  // namespace neomcp
