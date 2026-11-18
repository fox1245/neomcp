// What a tool handler sees of its call: cancellation, progress, logging, and who asked.
#pragma once

#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>

#include "neomcp/json.h"
#include "neomcp/model.h"
#include "neomcp/profile.h"
#include "neomcp/tool_host.h"

namespace neomcp {

namespace detail {
class ServerCore;
}  // namespace detail

// Thrown by a handler to answer its call with a tool execution error: a result with `isError` whose one text block
// is the message. Every other exception is answered with a fixed message (see ServerOptions::expose_error_details).
class ToolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The handle of one call, given to the handler that runs it.
//
// It is valid until the handler returns or, for a coroutine handler, until its coroutine finishes; do not keep it
// beyond that. Within that time it may be used from any thread, including threads the handler starts and joins
// before it returns.
class Context {
public:
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    // Stop is requested when the client cancels the call, when its session closes, and when the server is destroyed.
    // After that nothing more is written for the call, so the handler may give up at once.
    std::stop_token stop_token() const noexcept { return stop_; }
    bool cancelled() const noexcept { return stop_.stop_requested(); }

    // Sends a progress notification when the request asked for progress (it carried a progress token); a value that
    // is not greater than the last one sent is dropped. A no-op otherwise.
    void progress(double value, std::optional<double> total = std::nullopt,
                  std::optional<std::string> message = std::nullopt) const;

    // Sends a log message when the server has protocol logging on and the level is at or above the one in force
    // for the call (ServerOptions::protocol_logging). A no-op otherwise.
    void log(LogLevel level, json data, std::optional<std::string> logger = std::nullopt) const;
    void debug(json data, std::optional<std::string> logger = std::nullopt) const;
    void info(json data, std::optional<std::string> logger = std::nullopt) const;
    void warning(json data, std::optional<std::string> logger = std::nullopt) const;
    void error(json data, std::optional<std::string> logger = std::nullopt) const;

    // The request's params._meta; null when it had none.
    const json& request_meta() const noexcept { return meta_; }
    // The revision the call is served under, and its era. Behaviour that differs between revisions is the
    // library's job; these are for logging and diagnostics.
    const std::string& revision() const noexcept;
    profile::Era era() const noexcept;
    // From the handshake on a connection that has one, else from the request; null and {} when not given.
    const json& client_info() const noexcept;
    const json& client_capabilities() const noexcept;

private:
    friend class detail::ServerCore;
    Context(const CallControl& control, const json& meta, std::stop_token stop)
        : control_(control), meta_(meta), stop_(std::move(stop)) {}

    const CallControl& control_;
    const json& meta_;
    std::stop_token stop_;
};

}  // namespace neomcp
