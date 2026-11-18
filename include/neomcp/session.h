// The protocol engine of one connection, without I/O.
//
// A Session takes the lines a client sends, one JSON-RPC message (or batch) per line, and writes the lines a server
// answers through a sink. It owns no thread, socket, clock or file: the caller feeds it input and the tool host
// runs calls wherever it likes. It serves every enabled profile at once: a request that names its revision is
// served under that revision's profile without reading anything from the connection, and a handshake opens a
// connection served under the negotiated profile. Which way a message goes is decided by the mechanisms the
// profiles name, never by a revision string.
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
using Sink = std::function<void(std::string line)>;

// Checks tool arguments against the tool's input schema: a message when they fail it, empty when they pass.
using ArgumentCheck = std::function<std::optional<std::string>(const ToolDefinition& tool, const json& arguments)>;

// Caching hints a revision that has them requires on list and discovery results (`ttlMs`, `cacheScope`).
struct CacheHints {
    std::uint64_t ttl_ms = 0;                         // 0: stale at once
    CacheScope scope = CacheScope::private_scope;     // reuse only within the same authorization context
};

struct SessionOptions {
    // The revisions served; empty: every built-in profile. Two profiles of the same revision are an error.
    std::vector<profile::ProfilePtr> profiles;

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

    // Runs before every tools/call; empty: arguments are not checked.
    ArgumentCheck check_arguments;

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
    // The host must outlive the session. The sink is never called after close() returned.
    static std::variant<std::unique_ptr<Session>, CreateError> create(SessionOptions options, ToolHost& host,
                                                                      Sink sink);

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    // Closes the session.
    ~Session();

    // One input line. Called by one thread at a time. Ignored after close().
    void receive(std::string_view line);

    // Requests stop on every call in flight and drops everything not yet written. When it returns, nothing is
    // being written and nothing will be: it waits for a sink call in progress on another thread (but not for one
    // on its own thread, when close() is called from inside the sink).
    void close();

    // The enabled profiles, newest revision first.
    std::span<const profile::ProfilePtr> profiles() const noexcept;

private:
    explicit Session(std::shared_ptr<detail::SessionCore> core) : core_(std::move(core)) {}
    std::shared_ptr<detail::SessionCore> core_;
};

}  // namespace neomcp
