// The protocol engine of one connection, without I/O.
//
// A Session takes the lines a client sends, one JSON-RPC message (or batch) per line, and writes the lines a server
// answers through a sink. It owns no thread, socket, clock or file: the caller feeds it input and the tool host
// runs calls wherever it likes. It serves every enabled profile at once: a request that names its revision is
// served under that revision's profile without reading anything from the connection, and a handshake opens a
// connection served under the negotiated profile. Which way a message goes is decided by the mechanisms the
// profiles name, never by a revision string.
//
// A transport that answers each request on its own channel (as HTTP answers each POST) feeds the session input
// units: what one request of the transport carries, numbered by the transport, with the request's header fields.
// Every output then says which unit it belongs to, and every unit ends exactly once (see Output).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "neomcp/json.h"
#include "neomcp/mechanism.h"
#include "neomcp/model.h"
#include "neomcp/profile.h"
#include "neomcp/tool_host.h"

namespace neomcp {

namespace detail {
class SessionCore;
}  // namespace detail

// Receives each outgoing message serialised on one line, without the line terminator. It is called by one thread
// at a time, never re-entrantly, and never while the session holds a lock; it may call into the session.
//
// A sink that throws closes the session. The exception propagates out of Session::receive when the thread that
// reads was writing, including through a tool that completed or reported inline during receive(). A tool's own
// thread writing through a Completion or a CallControl never sees it: the sink's owner knows its write failed.
using Sink = std::function<void(std::string line)>;

// The number a transport gives an input unit when it feeds it (Session::receive): the message, or the batch where
// the connection allows one, that one request of the transport carries.
using UnitId = std::uint64_t;

enum class OutputKind {
    response,      // a response or an error answer, or the answer to a batch
    notification,  // a notification to the client
    request,       // a request to the client
    end            // no message: the unit ends without an answer
};

// Why an input was refused before any method ran. The name of each reason but `none` (to_string) is also a reason a
// revision's Streamable HTTP status map can give a status for (profile::declared_status_reasons).
enum class RefusalReason {
    none,
    parse_error,                 // not JSON
    invalid_request,             // not a JSON-RPC message, or one this connection does not take as it is
    too_long,                    // longer than SessionOptions::max_message_bytes
    too_deep,                    // nested deeper than SessionOptions::max_nesting_depth
    header_mismatch,             // a header the revision requires is missing or malformed, or contradicts the body
    unsupported_version,         // the body names a revision the session does not serve
    invalid_request_meta,        // the body's `_meta` lacks or misstates what the revision requires of it
    unsupported_version_header,  // the version header names a revision the session does not serve
    method_not_found,            // the revision has no such client request, or the session does not serve it
    missing_connection           // the request neither names its revision nor arrives on an opened connection
};

std::string_view to_string(RefusalReason reason) noexcept;

// One output of a session that writes to an OutputSink.
//
// Every input unit ends exactly once: with its answer (the response, or for a batch the batch's answer), whose
// `last` is set, or with an output of kind `end` when no answer will come: its request was cancelled, the session
// closed while the unit was open, or it held only notifications and responses. Nothing of a unit follows its last
// output. A unit the transport abandoned gets nothing more, its end included (Session::abandon); so does a unit fed
// after close(), and every unit still open when the sink threw or when close() was called from inside the sink.
struct Output {
    std::string line;            // the message on one line, without the line terminator; empty for kind `end`
    std::optional<UnitId> unit;  // the unit it belongs to; empty when it belongs to the connection
    bool last = false;           // the last output of its unit
    OutputKind kind = OutputKind::response;
    // Set when the input was refused before any method ran; the line is then the error answer.
    RefusalReason refusal = RefusalReason::none;
    // The profile that governs it: the one the input was served under or, for a refusal, the one whose `errors`
    // gave its code; null when none is known.
    profile::ProfilePtr profile;
};

// Receives each output, with the same guarantees as a Sink: one thread at a time, never re-entrantly, never under a
// lock of the session, and a sink that throws closes the session.
using OutputSink = std::function<void(Output output)>;

// One header field of the transport's request that carried an input unit: its name, and its value without the
// whitespace around it, as the transport got them. Names compare without regard to ASCII case, values exactly.
struct HeaderField {
    std::string_view name;
    std::string_view value;
};

// Called when the session becomes idle: no call it started waits for its answer (a call the client cancelled counts
// as answered from the moment of the cancellation, though its tool may still run), nothing waits to be written or is
// being written, and no batch is being assembled. It may run on any thread (the reading thread, a tool's thread or the
// thread that wrote last), possibly on two at once, with no lock of the session held, so that it may call into the
// session, `Session::idle()` included. It runs whenever an answer was written and nothing else is pending, which is
// more often than the session goes quiet for good. A callback that started before close() may still be running on
// another thread when close() returns, so whatever it touches must outlive the session. What it throws is swallowed.
// Keep it short: it runs on the thread that did the work.
using IdleCallback = std::function<void()>;

// Checks tool arguments against the tool's input schema: a message when they fail it, empty when they pass.
using ArgumentCheck = std::function<std::optional<std::string>(const ToolDefinition& tool, const json& arguments)>;

struct SessionOptions {
    // The revisions served; empty: every built-in profile. Two profiles of the same revision are an error.
    std::vector<profile::ProfilePtr> profiles;

    // The transport the session serves. A profile that does not define it is not served: no request can name it, no
    // handshake chooses it and no `supported` list names it; when no profile is left, the session is not created.
    // The cancellation mechanism, the request headers and the version header come from the profile's entry for it.
    profile::Transport transport = profile::Transport::stdio;

    Implementation server_info;
    std::optional<std::string> instructions;

    // On tools/list and server/discover results, when the revision's result type has the members.
    CacheHints tools_cache;
    // Tools per tools/list page; empty: one page with every tool. Zero is an error.
    std::optional<std::size_t> tools_page_size;

    // Log messages through the protocol (notifications/message). Off: the server declares no logging capability
    // and drops every log message.
    bool protocol_logging = false;
    // The level in force on a connection before the client sets one.
    LogLevel default_log_level = LogLevel::info;

    // Whether results carry the server's Implementation in `_meta`, when the revision defines that key.
    bool include_server_info = true;

    // A longer input line is answered with Invalid Request and not parsed. Default 8 MiB.
    std::size_t max_message_bytes = std::size_t{8} * 1024 * 1024;
    // A line whose arrays and objects nest deeper is answered with Invalid Request and not parsed. Default 128.
    std::size_t max_nesting_depth = 128;
    // When this many lines already wait to be written, a new progress or log notification is dropped (responses
    // and errors never are), so that a tool reporting faster than the client reads cannot grow the queue without
    // limit. Zero is an error. Default 1024.
    std::size_t max_queued_notifications = 1024;

    // Whether the internal error that answers a HostFailure carries the host's message. Off: the message is fixed,
    // so that nothing about the server's internals reaches the client.
    bool expose_error_details = false;

    // Runs before every tools/call; empty: arguments are not checked.
    ArgumentCheck check_arguments;

    // Told when the session becomes idle (see IdleCallback). Empty: nobody is told; Session::idle() answers the same
    // question on demand.
    IdleCallback on_idle;

    // Where mechanisms come from; null: builtin_mechanisms(). Must outlive the session's creation.
    const MechanismRegistry* mechanisms = nullptr;
};

// A mechanism a profile names that the registry has no implementation for.
struct MissingMechanism {
    std::string revision;
    MechanismSlot slot;
    std::string name;
};

// Why a session could not be created.
struct CreateError {
    std::string message;                    // every problem, one per line
    std::vector<MissingMechanism> missing;  // the missing mechanisms among them
};

class Session {
public:
    // The host must outlive the session. The sink is never called after close() returned. The sink gets the line of
    // every output that has one (it adapts the OutputSink of create_tagged).
    static std::variant<std::unique_ptr<Session>, CreateError> create(SessionOptions options, ToolHost& host,
                                                                      Sink sink);
    // The same, with every output given whole.
    static std::variant<std::unique_ptr<Session>, CreateError> create_tagged(SessionOptions options, ToolHost& host,
                                                                             OutputSink sink);

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    // Closes the session.
    ~Session();

    // One input line that belongs to no unit: its outputs belong to the connection, and no output of kind `end`
    // follows it. Called by one thread at a time. Ignored after close(). Throws what the sink threw when this thread
    // was writing (the session is closed then).
    void receive(std::string_view line);

    // One input unit, numbered by the caller, with the header fields of the request that carried it (they are read
    // during the call only). Called by one thread at a time, like the form above, and throws the same; feed a session
    // one form only. A number may be used again once its unit ended; a number whose unit is still open throws
    // std::invalid_argument.
    //
    // The headers are checked against the profiles' entries for the session's transport: the version header and the
    // request headers a revision defines. A header that is missing, repeated, malformed or contradicts the body, and a
    // version header naming a revision the session does not serve, refuse the input (Output::refusal). On a
    // transport whose entries define no headers, such as stdio, headers change nothing.
    //
    // Returns whether the unit awaits an answer: true when it holds a request or was refused (the answer may have
    // been written already, and a request that is cancelled later ends the unit without one), false when it holds
    // only notifications and responses. After close() the input is ignored and false returned.
    bool receive(UnitId unit, std::string_view input, std::span<const HeaderField> headers = {});

    // The transport lost the stream that carried the unit's outputs. The cancellation mechanism of the profile each
    // of the unit's calls is served under decides what happens to the call: it is stopped when the mechanism says a
    // closed stream cancels it, else it runs on. Either way nothing more of the unit is written, its end included.
    // When abandon() returns no output of the unit is being written and none will be: it waits for a sink call
    // writing one on another thread (but not on its own thread, when called from inside the sink). An unknown or
    // ended unit is ignored.
    void abandon(UnitId unit);

    // Requests stop on every call in flight and drops everything not yet written; each unit still open ends with an
    // output of kind `end`, written before close() returns. When it returns, nothing is being written and nothing
    // will be: it waits for a sink call in progress on another thread (but not for one on its own thread, when
    // close() is called from inside the sink: then the open units get no end).
    void close();

    // Whether the session is idle right now (see IdleCallback): true before the first message and after close().
    // Together with SessionOptions::on_idle this lets a transport wait for the calls in flight without polling:
    // check idle() after taking the lock the callback takes, and wait on a condition the callback signals.
    bool idle() const;

    // The enabled profiles that define the session's transport, newest revision first.
    std::span<const profile::ProfilePtr> profiles() const noexcept;

    // The profile a handshake opened the connection under; null while none did, and when the negotiated profile
    // keeps no connection. A transport reads from it what the connection needs, such as a session header.
    profile::ProfilePtr connection_profile() const;

private:
    explicit Session(std::shared_ptr<detail::SessionCore> core) : core_(std::move(core)) {}
    std::shared_ptr<detail::SessionCore> core_;
};

}  // namespace neomcp
