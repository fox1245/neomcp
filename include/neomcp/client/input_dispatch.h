// Runs the client's input handlers for a client engine, without I/O and without threads of its own.
//
// The dispatcher takes the engine's events that concern input: a request of the server (ServerAsked) and a round of a
// call whose result asks (InputRoundStarted) start the matching handlers; the end of either (ServerAskEnded,
// RequestEnded) requests stop on their handlers and drops what they still return. It gives the answers back to the
// engine (answer, answer_round, fail_round), which writes them for the revision.
//
// Where a handler runs is the runner's: the dispatcher hands it one job per handler run, and the runner runs the job
// and gives its outcome back with complete(). The client runs plain handlers on its bounded worker pool and coroutine
// handlers on its own thread; a test runs them at once, or holds them and completes them in an order it chooses. The
// jobs of one round are all started before any outcome is awaited, so a runner that allows it runs them concurrently;
// the answers go back once every job of the round has completed.
//
// Threads. Every method of the dispatcher is called on the engine's thread (the client's), and none is safe to call
// from two threads. A job's work touches nothing of the dispatcher or the engine: it may run on any thread, and the
// runner brings its outcome back to the dispatcher's thread before calling complete().
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>

#include <asio/awaitable.hpp>

#include "neomcp/client/engine.h"
#include "neomcp/client/input_handlers.h"
#include "neomcp/input.h"

namespace neomcp {

// One run of a handler. Never used twice.
struct InputJobId {
    std::uint64_t value = 0;
    friend bool operator==(const InputJobId&, const InputJobId&) = default;
    friend auto operator<=>(const InputJobId&, const InputJobId&) = default;
};

// A handler failed: it threw. `message` is the exception's text when it had one, for the application only; the
// server never sees it.
struct HandlerFailed {
    std::string message;
};

// What one run of a handler gave: its answer, the user's refusal (a sampling handler that returned InputRefused), or
// its failure.
using HandlerOutcome = std::variant<InputResponse, InputRefused, HandlerFailed>;

// One run of a handler, for a runner. Exactly one of `plain` and `coroutine` is set. The work owns everything it uses
// (the handler, a copy of the request, the context), so it may outlive the dispatcher, and it never throws: what the
// handler throws becomes HandlerFailed.
struct InputJob {
    InputJobId id;
    // Requested when the dispatcher no longer awaits the outcome (the request or the call ended); the handler's
    // InputContext sees the same stop. A runner may then drop a job that has not started, without completing it.
    std::stop_token stop;
    // A plain handler: run it on a worker thread; it returns when the handler returned.
    std::function<HandlerOutcome()> plain;
    // A coroutine handler: run it on the client's thread (co_spawn on the client's executor).
    std::function<asio::awaitable<HandlerOutcome>()> coroutine;
};

// Where handlers run. See the top of this file.
class InputRunner {
public:
    virtual ~InputRunner() = default;
    // Takes a job, runs it now or later, and once it ran calls InputDispatcher::complete with its id and outcome on
    // the dispatcher's thread. It may call complete from inside start: the outcome then takes effect once the
    // dispatcher's call that started the job has returned.
    virtual void start(InputJob job) = 0;
};

// Something about the handlers the application should hear of, besides the ends of its calls.
enum class InputReportKind {
    handler_failed  // a handler threw: a server's request was answered with an internal error, or a round's call
                    // failed with handler_failed
};

struct InputReport {
    InputReportKind kind = InputReportKind::handler_failed;
    std::optional<InputRef> request;  // the server's request, when it was one
    std::optional<RequestRef> call;   // the call, when the request belongs to one the wire names
    std::optional<std::string> key;   // the key in the round, when a call's result asked
    std::string message;              // the exception's text, when it had one
};

class InputDispatcher {
public:
    // Dispatches for `engine` with `handlers` through `runner`; both must outlive the dispatcher. The engine must have
    // been created with `handlers.support()` as its EngineOptions::input, so that what the client declares and what it
    // has handlers for are the same; std::invalid_argument otherwise.
    InputDispatcher(ClientEngine& engine, InputHandlers handlers, InputRunner& runner);
    InputDispatcher(const InputDispatcher&) = delete;
    InputDispatcher& operator=(const InputDispatcher&) = delete;
    // Requests stop on every job whose outcome is still awaited.
    ~InputDispatcher();

    // Takes one event of the engine. ServerAsked and InputRoundStarted start the handlers they need; ServerAskEnded
    // and RequestEnded request stop on the handlers of the request or call that ended. Returns true for the events
    // that concern only input (ServerAsked, ServerAskEnded, InputRoundStarted); false for the others, RequestEnded
    // included, which the caller still delivers to the application.
    bool take(const EngineEvent& event);

    // The outcome of a job, on the dispatcher's thread. Ignored for a job whose request or call ended, and for a job
    // that already completed.
    void complete(InputJobId job, HandlerOutcome outcome);

    // The next report, oldest first; empty when none is queued.
    std::optional<InputReport> next_report();

    // Jobs started whose outcome is still awaited.
    std::size_t outstanding() const noexcept;

    // Requests stop on every outstanding job and ignores later events and outcomes (the client closes).
    void close();

private:
    std::unique_ptr<detail::InputDispatcherCore> core_;
};

}  // namespace neomcp
