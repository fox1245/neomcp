// The client's input handlers (neomcp/client/input_handlers.h) and the dispatcher that runs them for an engine
// (neomcp/client/input_dispatch.h).
#include "neomcp/client/input_dispatch.h"

#include <deque>
#include <exception>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace neomcp {

InputSupport InputHandlers::support() const noexcept {
    InputSupport support;
    support.form = static_cast<bool>(form_);
    support.url = static_cast<bool>(url_);
    support.sampling = static_cast<bool>(sampling_);
    support.sampling_tools = support.sampling && sampling_support_.tools;
    support.sampling_context = support.sampling && sampling_support_.context;
    support.roots = static_cast<bool>(roots_);
    support.roots_list_changed = support.roots && roots_notify_;
    return support;
}

bool InputHandlers::empty() const noexcept { return !form_ && !url_ && !sampling_ && !roots_; }

namespace detail {

namespace {

// What a server is told when a handler failed: the exception's text never leaves the client.
constexpr std::string_view handler_failed_message = "The client could not answer the request";
// What a server is told when no handler takes a request the engine passed on, which only an engine created with other
// handlers than the dispatcher's does.
constexpr std::string_view no_handler_message = "The client has no handler for the request";

HandlerOutcome outcome_of(ElicitationAnswer answer) { return InputResponse(std::move(answer)); }
HandlerOutcome outcome_of(RootsAnswer answer) { return InputResponse(std::move(answer)); }
HandlerOutcome outcome_of(SamplingReply reply) {
    if (auto* answer = std::get_if<SamplingAnswer>(&reply)) return InputResponse(std::move(*answer));
    return InputRefused{};
}

HandlerOutcome failed_outcome(const std::exception_ptr& thrown) {
    try {
        std::rethrow_exception(thrown);
    } catch (const std::exception& error) {
        return HandlerFailed{error.what()};
    } catch (...) {
        return HandlerFailed{};
    }
}

// A coroutine handler's run. Its parameters are values, so the frame owns what the handler uses whatever happens to
// the function object that started it.
template <class Request, class Answer>
asio::awaitable<HandlerOutcome> run_coroutine(std::shared_ptr<CoroutineInputBody<Request, Answer>> body,
                                              Request request, std::shared_ptr<InputContext> context) {
    std::exception_ptr thrown;
    try {
        Answer answer = co_await body->run(std::move(request), *context);
        co_return outcome_of(std::move(answer));
    } catch (...) {
        thrown = std::current_exception();
    }
    co_return failed_outcome(thrown);
}

}  // namespace

class InputDispatcherCore {
public:
    InputDispatcherCore(ClientEngine& engine, InputHandlers handlers, InputRunner& runner)
        : engine_(engine), handlers_(std::move(handlers)), runner_(runner) {
        if (!(engine_.input() == handlers_.support())) {
            throw std::invalid_argument(
                "the input dispatcher's handlers are not those the engine declares (EngineOptions::input)");
        }
    }

    ~InputDispatcherCore() { stop_all(); }

    bool take(const EngineEvent& event) {
        if (closed_) {
            return std::holds_alternative<ServerAsked>(event) || std::holds_alternative<ServerAskEnded>(event) ||
                   std::holds_alternative<InputRoundStarted>(event);
        }
        Busy busy(*this);
        if (const auto* asked = std::get_if<ServerAsked>(&event)) {
            on_asked(*asked);
            return true;
        }
        if (const auto* ended = std::get_if<ServerAskEnded>(&event)) {
            on_ask_ended(*ended);
            return true;
        }
        if (const auto* round = std::get_if<InputRoundStarted>(&event)) {
            on_round(*round);
            return true;
        }
        if (const auto* call = std::get_if<RequestEnded>(&event)) on_call_ended(call->request);
        return false;
    }

    void complete(InputJobId id, HandlerOutcome outcome) {
        if (closed_) return;
        if (busy_ > 0) {
            deferred_.emplace_back(id, std::move(outcome));
            return;
        }
        Busy busy(*this);
        apply(id, std::move(outcome));
    }

    std::optional<InputReport> next_report() {
        if (reports_.empty()) return std::nullopt;
        InputReport report = std::move(reports_.front());
        reports_.pop_front();
        return report;
    }

    std::size_t outstanding() const noexcept { return jobs_.size(); }

    void close() {
        closed_ = true;
        stop_all();
        deferred_.clear();
    }

private:
    // Who waits for a job's outcome: a server's request, or one key of a call's round.
    struct Job {
        std::stop_source stop;
        std::optional<InputRef> request;    // the server's request it answers
        std::optional<RequestRef> round;    // the call whose round it answers
        std::optional<std::string> key;     // its key in that round
        std::optional<RequestRef> call;     // the call it belongs to, as the context and a report name it
    };

    struct Round {
        std::vector<InputJobId> jobs;
        InputResponses answers;
        std::size_t waiting = 0;
    };

    // Outcomes that arrive while the dispatcher is inside one of its own calls (a runner that runs a job inside
    // start) take effect when that call returns, so that a round has started all its jobs before any of them ends it.
    class Busy {
    public:
        explicit Busy(InputDispatcherCore& core) : core_(core) { ++core_.busy_; }
        Busy(const Busy&) = delete;
        Busy& operator=(const Busy&) = delete;
        ~Busy() {
            if (--core_.busy_ > 0) return;
            ++core_.busy_;
            while (!core_.deferred_.empty() && !core_.closed_) {
                auto [id, outcome] = std::move(core_.deferred_.front());
                core_.deferred_.pop_front();
                core_.apply(id, std::move(outcome));
            }
            --core_.busy_;
        }

    private:
        InputDispatcherCore& core_;
    };

    // ---- starting

    template <class Request, class Answer>
    void start(const InputBody<Request, Answer>& body, Request request, Job job, const std::string& method,
               const profile::ProfilePtr& profile) {
        const InputJobId id{next_job_++};
        std::shared_ptr<InputContext> context(new InputContext(job.stop.get_token(), method,
                                                               profile != nullptr ? profile->revision() : std::string(),
                                                               job.call, job.key));
        InputJob work;
        work.id = id;
        work.stop = job.stop.get_token();
        if (body.plain) {
            work.plain = [plain = body.plain, owned = std::move(request), context]() -> HandlerOutcome {
                try {
                    return outcome_of(plain->run(owned, *context));
                } catch (...) {
                    return failed_outcome(std::current_exception());
                }
            };
        } else {
            work.coroutine = [coroutine = body.coroutine, owned = std::move(request), context]() {
                return run_coroutine<Request, Answer>(coroutine, owned, context);
            };
        }
        if (job.request) asks_[*job.request] = id;
        if (job.round) rounds_.at(*job.round).jobs.push_back(id);
        jobs_.emplace(id, std::move(job));
        runner_.start(std::move(work));
    }

    // The handler of a kind of request.
    template <class T>
    const auto& body_for() const {
        if constexpr (std::is_same_v<T, ElicitationForm>) {
            return handlers_.form_;
        } else if constexpr (std::is_same_v<T, ElicitationUrl>) {
            return handlers_.url_;
        } else if constexpr (std::is_same_v<T, SamplingRequest>) {
            return handlers_.sampling_;
        } else {
            return handlers_.roots_;
        }
    }

    // Starts the handler of the request's kind; false when there is none.
    bool start_for(const InputRequest& request, Job job, const std::string& method, const profile::ProfilePtr& profile) {
        return std::visit(
            [&](const auto& value) -> bool {
                using T = std::decay_t<decltype(value)>;
                const auto& body = body_for<T>();
                if (!body) return false;
                start(body, value, std::move(job), method, profile);
                return true;
            },
            request);
    }

    void on_asked(const ServerAsked& asked) {
        Job job;
        job.request = asked.ref;
        job.call = asked.call;
        if (!start_for(asked.request, std::move(job), asked.method, asked.profile)) {
            engine_.answer(asked.ref, RequestError{error_code::internal_error, std::string(no_handler_message)});
        }
    }

    void on_ask_ended(const ServerAskEnded& ended) {
        const auto found = asks_.find(ended.ref);
        if (found == asks_.end()) return;
        const InputJobId id = found->second;
        asks_.erase(found);
        drop(id);
    }

    void on_round(const InputRoundStarted& round) {
        if (rounds_.count(round.request) != 0) on_call_ended(round.request);
        if (round.asking.requests.empty()) {
            // A round that asks for nothing but its state goes out again at once.
            engine_.answer_round(round.request, {});
            return;
        }
        Round& started = rounds_[round.request];
        started.waiting = round.asking.requests.size();
        for (const AskedInput& one : round.asking.requests) {
            Job job;
            job.round = round.request;
            job.call = round.request;
            job.key = one.key;
            if (!start_for(one.request, std::move(job), one.method, round.profile)) {
                fail_round(round.request, CallFailure{CallFailureKind::server_asked_undeclared,
                                                      "the client has no handler for a request of the round"});
                return;
            }
        }
    }

    void on_call_ended(RequestRef call) {
        const auto found = rounds_.find(call);
        if (found == rounds_.end()) return;
        const std::vector<InputJobId> ids = std::move(found->second.jobs);
        rounds_.erase(found);
        for (const InputJobId id : ids) drop(id);
    }

    // ---- outcomes

    void apply(InputJobId id, HandlerOutcome outcome) {
        const auto found = jobs_.find(id);
        if (found == jobs_.end()) return;
        Job job = std::move(found->second);
        jobs_.erase(found);
        if (auto* failed = std::get_if<HandlerFailed>(&outcome)) {
            reports_.push_back(InputReport{InputReportKind::handler_failed, job.request, job.call, job.key,
                                           std::move(failed->message)});
        }
        if (job.request) {
            asks_.erase(*job.request);
            answer_request(*job.request, std::move(outcome));
            return;
        }
        if (!job.round) return;
        const auto round = rounds_.find(*job.round);
        if (round == rounds_.end()) return;
        if (std::holds_alternative<HandlerFailed>(outcome)) {
            fail_round(*job.round, CallFailure{CallFailureKind::handler_failed, "a handler of the round failed"});
            return;
        }
        if (std::holds_alternative<InputRefused>(outcome)) {
            fail_round(*job.round, CallFailure{CallFailureKind::input_refused, "the user refused a request of the round"});
            return;
        }
        round->second.answers.emplace(*job.key, std::get<InputResponse>(std::move(outcome)));
        if (--round->second.waiting > 0) return;
        InputResponses answers = std::move(round->second.answers);
        const RequestRef call = *job.round;
        rounds_.erase(round);
        engine_.answer_round(call, std::move(answers));
    }

    void answer_request(InputRef request, HandlerOutcome outcome) {
        if (auto* response = std::get_if<InputResponse>(&outcome)) {
            engine_.answer(request, std::move(*response));
        } else if (std::holds_alternative<InputRefused>(outcome)) {
            engine_.answer(request, InputRefused{});
        } else {
            engine_.answer(request, RequestError{error_code::internal_error, std::string(handler_failed_message)});
        }
    }

    // Ends a round with a failure: the other handlers of the round are stopped, and the call ends.
    void fail_round(RequestRef call, CallFailure failure) {
        on_call_ended(call);
        engine_.fail_round(call, std::move(failure));
    }

    // Stops a job whose outcome is no longer wanted.
    void drop(InputJobId id) {
        const auto found = jobs_.find(id);
        if (found == jobs_.end()) return;
        found->second.stop.request_stop();
        jobs_.erase(found);
    }

    void stop_all() {
        for (auto& [id, job] : jobs_) job.stop.request_stop();
        jobs_.clear();
        asks_.clear();
        rounds_.clear();
    }

    ClientEngine& engine_;
    InputHandlers handlers_;
    InputRunner& runner_;
    std::uint64_t next_job_ = 1;
    std::map<InputJobId, Job> jobs_;
    std::map<InputRef, InputJobId> asks_;
    std::map<RequestRef, Round> rounds_;
    std::deque<InputReport> reports_;
    std::deque<std::pair<InputJobId, HandlerOutcome>> deferred_;
    int busy_ = 0;
    bool closed_ = false;
};

}  // namespace detail

InputDispatcher::InputDispatcher(ClientEngine& engine, InputHandlers handlers, InputRunner& runner)
    : core_(std::make_unique<detail::InputDispatcherCore>(engine, std::move(handlers), runner)) {}
InputDispatcher::~InputDispatcher() = default;

bool InputDispatcher::take(const EngineEvent& event) { return core_->take(event); }
void InputDispatcher::complete(InputJobId job, HandlerOutcome outcome) { core_->complete(job, std::move(outcome)); }
std::optional<InputReport> InputDispatcher::next_report() { return core_->next_report(); }
std::size_t InputDispatcher::outstanding() const noexcept { return core_->outstanding(); }
void InputDispatcher::close() { core_->close(); }

}  // namespace neomcp
