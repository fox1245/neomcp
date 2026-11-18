// The interface through which a session reaches tools, and through them the other things a server offers.
//
// A session lists tools and calls them through a ToolHost. A call receives a CallControl, the handle through which
// the tool sees cancellation and reports progress and log messages, and a Completion, which it calls exactly once
// with the outcome, inline or later from any thread. Both are copyable handles that stay safe to use after the
// session closed or was destroyed: anything they report then is dropped.
#pragma once

#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <variant>

#include "neomcp/json.h"
#include "neomcp/model.h"
#include "neomcp/profile.h"

namespace neomcp {

namespace detail {
class SessionCore;
struct CallState;
struct CompletionState;
}  // namespace detail

struct CallRequest {
    std::string name;
    json arguments = json::object();  // an object; empty when the request had none
    json meta = nullptr;              // the request's params._meta; null when absent
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

using CallOutcome = std::variant<ToolResult, Refusal, HostFailure>;

// The URI of a resources/read names nothing the host offers. It is answered with the error the revision prescribes
// for a resource that does not exist.
struct NotFound {
    std::string uri;
};

// The host found the request itself invalid (a prompt that does not exist, an argument it cannot use). It is
// answered with Invalid params and `message`.
struct InvalidRequest {
    std::string message;
};

// What a call of a kind other than a tool produced. Refusal and HostFailure stay in CallOutcome and mean the same
// for every kind of call.
using FeatureOutcome = std::variant<ReadResult, PromptResult, NotFound, InvalidRequest>;

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

private:
    friend class detail::SessionCore;
    explicit CallControl(std::shared_ptr<detail::CallState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::CallState> state_;
};

// Completes one call. Copies share one completion: the first call counts and later ones are ignored. When the
// last copy is destroyed without having been called, the call is answered with an internal error. Completing never
// throws what the session's sink throws: a failing sink closes the session, and the tool's thread goes on.
class Completion {
public:
    // A tool's outcome, or the refusal or the failure of a call of any kind. A tool result that completes a call
    // of another kind is a defect of the host and is answered with an internal error.
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

class ToolHost {
public:
    virtual ~ToolHost() = default;

    // The other things the host offers, each null when it offers none of its kind; a session declares the matching
    // capability exactly when the pointer is not null, and answers the methods of a kind the host lacks as "method
    // not found". The pointers stay valid as long as the host does. A host that offers only tools overrides none.
    virtual ResourceHost* resource_host() noexcept { return nullptr; }
    virtual PromptHost* prompt_host() noexcept { return nullptr; }

    // The tools, in a stable order. The span stays valid and its contents unchanged while a session uses the host.
    virtual std::span<const ToolDefinition> tools() const = 0;

    // Runs the tool `request.name`, one of tools(). Must not block for the duration of the tool: a session calls
    // it on the thread that reads its input. An exception that escapes is treated as the call never completing:
    // `done` then answers with an internal error once its last copy is gone.
    virtual void call(const CallRequest& request, CallControl control, Completion done) = 0;
};

}  // namespace neomcp
