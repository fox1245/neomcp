// The interface through which a session reaches tools.
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

using CallOutcome = std::variant<ToolResult, Refusal>;

// The handle of one call in flight.
class CallControl {
public:
    // Stop is requested when the client cancels the call or the session closes.
    std::stop_token stop_token() const noexcept;

    // Sends a progress notification when the request asked for progress; a value that is not finite or not greater
    // than the last one sent is dropped, and so is anything after the call completed or was cancelled.
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
// last copy is destroyed without having been called, the call is answered with an internal error.
class Completion {
public:
    void operator()(CallOutcome outcome) const;

private:
    friend class detail::SessionCore;
    explicit Completion(std::shared_ptr<detail::CompletionState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::CompletionState> state_;
};

class ToolHost {
public:
    virtual ~ToolHost() = default;

    // The tools, in a stable order. The span stays valid and its contents unchanged while a session uses the host.
    virtual std::span<const ToolDefinition> tools() const = 0;

    // Runs the tool `request.name`, one of tools(). Must not block for the duration of the tool: a session calls
    // it on the thread that reads its input. An exception that escapes is treated as the call never completing:
    // `done` then answers with an internal error once its last copy is gone.
    virtual void call(const CallRequest& request, CallControl control, Completion done) = 0;
};

}  // namespace neomcp
