// What a tool handler sees of its call: cancellation, progress, logging, who asked, and the answers to what it asked.
#pragma once

#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>

#include "neomcp/authorization.h"
#include "neomcp/input.h"
#include "neomcp/json.h"
#include "neomcp/model.h"
#include "neomcp/profile.h"
#include "neomcp/tool_host.h"

namespace neomcp {

namespace detail {
class ServerCore;
class InputRound;
struct TaskRun;
}  // namespace detail

// Thrown by a handler to answer its call with a tool execution error: a result with `isError` whose one text block
// is the message. Every other exception is answered with a fixed message (see ServerOptions::expose_error_details).
class ToolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Thrown by a handler of any kind (a tool, a resource or a template, a prompt, a completer; plain or coroutine) to
// answer its request with a JSON-RPC error of its own: the answer carries exactly this code, message and data (no
// `data` member when it is null), on every transport and under every revision. For a call that runs as a task, the
// task fails with them.
//
// The server answers it only when the code may be used by an application: a code outside the range JSON-RPC reserves
// for itself (-32768 to -32000), or one of -32099 to -32000, which that range leaves to implementations, that the
// revision serving the request does not name (the `errors` of its profile and of the extensions in force, and the
// codes it forbids a sender to use). The protocol's own codes are inside the reserved range. Any other code is
// not sent: the request is answered with an internal error whose message is fixed, and the message and the data are
// dropped. So is an answer longer than ServerOptions::max_message_bytes (the whole JSON-RPC message); for a task, the
// bound is the task store's (MemoryTaskStoreOptions::max_result_bytes on the error object), and the task then fails as an
// internal error. The message and the data reach the client as they are, so they must hold nothing the client may not
// see.
class ApplicationError : public std::runtime_error {
public:
    ApplicationError(int code, std::string message, json data = nullptr)
        : std::runtime_error(std::move(message)), code_(code), data_(std::move(data)) {}

    int code() const noexcept { return code_; }
    const json& data() const noexcept { return data_; }

private:
    int code_;
    json data_;
};

// Thrown by a resource handler when what its URI names does not exist (for example a template matched, and nothing is
// stored under the name it carries). The read is answered as the revision prescribes for a resource that does not
// exist. Any other exception is answered with an internal error whose text stays on the server
// (ServerOptions::expose_error_details adds it).
class ResourceNotFound : public std::runtime_error {
public:
    ResourceNotFound() : std::runtime_error("resource not found") {}
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
    // After that nothing more is written for the call, so the handler may give up at once. For a call that runs as a
    // task, stop is requested when the task is cancelled, when its lifetime ends and when the server is destroyed; the
    // end of the request or session that started it does not request it.
    std::stop_token stop_token() const noexcept { return stop_; }
    bool cancelled() const noexcept { return stop_.stop_requested(); }

    // The id of the task, when the call runs as one (see "Tasks" in neomcp/server.h); empty otherwise. Valid as long
    // as this Context.
    std::optional<std::string_view> task_id() const noexcept;
    // Sets the status message of the task, which the requester sees when it asks about the task; the task stays
    // working. Dropped when the call does not run as a task, and once the task has ended.
    void task_status(std::string message) const;

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
    // Who asked, as the transport validated it (see neomcp/authorization.h): read only, valid as long as this Context,
    // never the token. Null where nothing is authorized (stdio, in process).
    const Authorization* authorization() const noexcept;

    // When the handler asked for input before (it returned neomcp::InputRequired): the answers of the last round,
    // by the keys it chose, and the state it gave with its requests. Both empty on the first entry. Only the last
    // round's answers come back; a handler that needs earlier ones keeps them in its state.
    const InputResponses& input_responses() const noexcept { return responses_; }
    const std::optional<std::string>& request_state() const noexcept { return state_; }
    // Whether the client can take `request` now: the revision carries it and the client declared what it needs. A
    // handler that can do without asks something else when it cannot (asking anyway fails the call).
    bool client_supports(const InputRequest& request) const;

private:
    friend class detail::ServerCore;
    friend class detail::InputRound;  // the declared inputs of neomcp/server/inputs.h: the call's profile
    Context(const CallControl& control, const json& meta, std::stop_token stop, const InputResponses& responses,
            const std::optional<std::string>& state, const detail::TaskRun* task = nullptr)
        : control_(control), meta_(meta), stop_(std::move(stop)), responses_(responses), state_(state), task_(task) {}

    const CallControl& control_;
    const json& meta_;
    std::stop_token stop_;
    const InputResponses& responses_;
    const std::optional<std::string>& state_;
    const detail::TaskRun* task_;  // the task the call runs as; null for a call
};

}  // namespace neomcp
