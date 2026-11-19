// The interface through which a session reaches tools, and through them the other things a server offers.
//
// A session lists tools and calls them through a ToolHost. A call receives a CallControl, the handle through which
// the tool sees cancellation and reports progress and log messages, and a Completion, which it calls exactly once
// with the outcome, inline or later from any thread. Both are copyable handles that stay safe to use after the
// session closed or was destroyed: anything they report then is dropped.
//
// A call may also end with InputRequired (neomcp/input.h): the host needs something only the client can give. The
// session asks the client and calls the host again with the answers, in a new call() with a new Completion (and the
// same CallControl); the host decides from the answers and its state what is still missing.
//
// What a host offers may change while it serves: an item can be switched off (a tool, a prompt, a resource or a
// template that is not enabled is not listed and is answered as one that does not exist), and a resource's content can
// change. A host that reports such changes says which kinds (change_support) and takes the listeners of the sessions
// that tell their clients (watch); see change.h.
#pragma once

#include <cstddef>

#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <variant>

#include "neomcp/change.h"
#include "neomcp/input.h"
#include "neomcp/json.h"
#include "neomcp/model.h"
#include "neomcp/profile.h"

namespace neomcp {

namespace detail {
class SessionCore;
struct CallState;
struct CompletionState;
struct WaitState;
}  // namespace detail

struct CallRequest {
    std::string name;
    json arguments = json::object();  // an object; empty when the request had none
    json meta = nullptr;              // the request's params._meta; null when absent
    // When the host asked for input before (InputRequired): the answers of the last round by key, and the state the
    // host gave with its requests. Empty on the first round.
    InputResponses input_responses;
    std::optional<std::string> request_state;
};

// Why a host did not run a call.
enum class Refusal {
    busy  // no capacity to run it now
};

// The host could not produce a result: a defect of the server program rather than of the call, such as a structured
// result that does not match the tool's own output schema. It is answered with an internal error, which carries
// `message` only when SessionOptions::expose_error_details is on.
struct HostFailure {
    std::string message;
};

// A tool's outcome, or what a call of any kind may end with: a refusal, a failure, or a request for input. Input may
// be asked for by tools, prompts and resource reads; a completion that asks is answered with an internal error.
using CallOutcome = std::variant<ToolResult, Refusal, HostFailure, InputRequired>;

// The URI of a resources/read names nothing the host offers. It is answered with the error the revision prescribes
// for a resource that does not exist.
struct NotFound {
    std::string uri;
};

// The host found the request itself invalid (a prompt that does not exist, an argument it cannot use, answers to
// its input requests that it cannot use). It is answered with Invalid params and `message`, whatever the kind of the
// call: a tool's call too, completed with it as a FeatureOutcome.
struct InvalidRequest {
    std::string message;
};

// What a call of a kind other than a tool produced. Refusal and HostFailure stay in CallOutcome and mean the same
// for every kind of call.
using FeatureOutcome = std::variant<ReadResult, PromptResult, CompletionResult, NotFound, InvalidRequest>;

// The handle of one call in flight.
class CallControl {
public:
    // Stop is requested when the client cancels the call or the session closes.
    std::stop_token stop_token() const noexcept;

    // Neither progress() nor log() throws what the session's sink throws; a failing sink closes the session.
    //
    // Sends a progress notification when the request asked for progress; a value that is not finite or not greater
    // than the last one sent is dropped, and so is anything after the call completed or was cancelled, and any
    // notification while the session's output queue is full (SessionOptions::max_queued_notifications). A dropped
    // value does not count as sent.
    void progress(double progress, std::optional<double> total = std::nullopt,
                  std::optional<std::string> message = std::nullopt) const;
    // Sends a log message when protocol logging is on and the level is at or above the one in force for the call.
    void log(LogLevel level, json data, std::optional<std::string> logger = std::nullopt) const;

    // The profile the call is served under.
    const profile::Profile& profile() const noexcept;
    // From the handshake on a connection that has one, else from the request; null and {} when not given.
    const json& client_info() const noexcept;
    const json& client_capabilities() const noexcept;

    // Whether the call may ask the client for `request`: the revision can carry it and the client declared the
    // capability it needs (for an elicitation, the mode; for sampling, tools and context where the request uses
    // them). A call that asks for a request the client cannot take fails as the revision prescribes, so a host that
    // can do without asks something else instead.
    bool client_supports(const InputRequest& request) const;

private:
    friend class detail::SessionCore;
    explicit CallControl(std::shared_ptr<detail::CallState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::CallState> state_;
};

// A call that waits for its client's answers to the requests it sent (under the mechanism that sends them as requests
// to the client). The session owns no clock: whoever wants a time-out keeps one and calls expire() when it lapses. A
// copyable handle that stays safe to use after the wait ended and after the session closed.
class InputWait {
public:
    // Stop is requested when the wait ends, however it ends: every answer arrived, the call was cancelled, its unit
    // was abandoned, the session closed, or expire() was called.
    std::stop_token ended() const noexcept;
    // Ends the wait as timed out: the call fails as a call fails whose client did not answer (see Session), and
    // nothing more is sent for it. Ignored when the wait has ended already.
    void expire() const;

private:
    friend class detail::SessionCore;
    explicit InputWait(std::shared_ptr<detail::WaitState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::WaitState> state_;
};

// Completes one call. Copies share one completion: the first call counts and later ones are ignored. When the
// last copy is destroyed without having been called, the call is answered with an internal error. Completing never
// throws what the session's sink throws: a failing sink closes the session, and the tool's thread goes on.
class Completion {
public:
    // A tool's outcome, or the refusal, the failure or the request for input of a call of any kind. A tool result
    // that completes a call of another kind is a defect of the host and is answered with an internal error.
    void operator()(CallOutcome outcome) const;
    // The outcome of a call of another kind than a tool. An outcome that does not belong to the call's kind (a read
    // result for a prompt) is a defect of the host and is answered with an internal error.
    void operator()(FeatureOutcome outcome) const;

private:
    friend class detail::SessionCore;
    explicit Completion(std::shared_ptr<detail::CompletionState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::CompletionState> state_;
};

class ResourceHost;
class PromptHost;
class CompletionHost;

class ToolHost {
public:
    virtual ~ToolHost() = default;

    // The other things the host offers, each null when it offers none of its kind; a session declares the matching
    // capability exactly when the pointer is not null, and answers the methods of a kind the host lacks as "method
    // not found". The pointers stay valid as long as the host does. A host that offers only tools overrides none.
    virtual ResourceHost* resource_host() noexcept { return nullptr; }
    virtual PromptHost* prompt_host() noexcept { return nullptr; }
    virtual CompletionHost* completion_host() noexcept { return nullptr; }

    // The tools, in a stable order. The span stays valid and its contents unchanged while a session uses the host.
    virtual std::span<const ToolDefinition> tools() const = 0;
    // Whether the tool at `index` of tools() is offered now. One that is not is left out of tools/list and a call of
    // it is answered as a call of a tool that does not exist. May change at any time; thread-safe. Default: every
    // tool is.
    virtual bool tool_enabled(std::size_t index) const {
        static_cast<void>(index);
        return true;
    }

    // The kinds of change the host reports to the listeners registered with watch(). Default: none.
    virtual ChangeSupport change_support() const { return {}; }
    // Registers a listener for the host's changes, which the host holds weakly until the registration is reset or
    // destroyed; an empty registration when the host reports no changes (or no more: it has stopped). The host tells
    // a listener of every change it reports after watch() returned, through ChangeListener::changed, and that it stops,
    // through ChangeListener::ended. Called with no lock of the session held; thread-safe. Default: an empty
    // registration.
    virtual ChangeRegistration watch(std::weak_ptr<ChangeListener> listener) {
        static_cast<void>(listener);
        return {};
    }

    // Runs the tool `request.name`, one of tools() that tool_enabled() admitted. Must not block for the duration of the
    // tool: a session calls it on the thread that reads its input (and, for a call that asked for input, on the thread
    // that delivered the last answer). An exception that escapes is treated as the call never completing: `done` then
    // answers with an internal error once its last copy is gone.
    virtual void call(const CallRequest& request, CallControl control, Completion done) = 0;

    // Told that a call waits for its client's answers (see InputWait): after the session has handed the round's
    // requests to its sink, on the thread that completed the round, with no lock of the session held. Must not
    // block. The default keeps no time-out: the call waits until it is answered or cancelled, or the session closes.
    virtual void awaiting_input(InputWait wait) { static_cast<void>(wait); }
};

}  // namespace neomcp
