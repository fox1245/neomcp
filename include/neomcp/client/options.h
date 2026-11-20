// The options of a client and of one call.
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "neomcp/argument_headers.h"
#include "neomcp/client/engine.h"
#include "neomcp/client/input_dispatch.h"
#include "neomcp/client/input_handlers.h"
#include "neomcp/client/mechanism.h"
#include "neomcp/client/methods.h"
#include "neomcp/client/timers.h"
#include "neomcp/model.h"
#include "neomcp/profile.h"

namespace neomcp {

// ---- One call

// The server's report of how far a call has come.
struct Progress {
    double progress = 0;
    std::optional<double> total;
    std::optional<std::string> message;
};

struct RequestOptions {
    // How long the call waits for its answer; empty: ClientOptions::request_timeout. Zero: the call fails at once with
    // invalid_options. The clock runs only while the client waits for the server: it waits while an input handler
    // answers the server.
    std::optional<std::chrono::milliseconds> timeout;
    // Each progress report starts the wait again, never beyond ClientOptions::max_total_timeout in all.
    bool reset_timeout_on_progress = false;
    // Told of each progress report of the call, on the client's thread; it must return promptly. Set: the call asks
    // the server for progress reports.
    std::function<void(const Progress& progress)> on_progress;
    // Stop requested: the call is cancelled as its transport prescribes and ends `cancelled`. Cancelling the coroutine
    // that awaits an async_ method does the same.
    std::stop_token stop;
};

// ---- What the client tells the application besides the ends of its calls

// The connection ended and was made again for the next request: on stdio the server process ended and a new one was
// started; on the older transport of events over HTTP the event stream ended and a new one was opened. Whatever the
// server kept is gone; where the revision opens with a handshake, the handshake was made again.
struct ServerRestarted {
    std::size_t restarts = 0;      // restarts since the client opened
    bool handshake_repeated = false;
};

// A tool the server listed was left out of the list: its input schema marks parameters for HTTP headers in a way the
// rules forbid (header_parameters, neomcp/argument_headers.h), on a transport that mirrors marked arguments into
// headers. `tool` is the name the server gave it, `issue` the rule it breaks and where. Such a tool is not offered to
// the application, and the client keeps no definition of it: a call of it by name still goes out, without mirrored
// arguments, after the tools were listed again to look for its definition. On a transport without such headers the
// marks are ignored.
struct ToolDropped {
    std::string tool;
    HeaderMarkIssue issue;
};

using ClientDiagnostic = std::variant<Diagnostic, InputReport, ServerRestarted, ToolDropped>;

// ---- The client

// The requests whose answer, once lost (server_exited, stream_lost), the client asks for again once by itself: the
// reads and the lists, which a server serves without changing anything. A tool call is not among them: it may have
// run, so it ends with the error and the application decides.
inline constexpr std::array<std::string_view, 6> default_resent_methods{
    client_method::tools_list,   client_method::resources_list, client_method::resources_templates_list,
    client_method::resources_read, client_method::prompts_list,  client_method::prompts_get};

// The options of a Client. The protocol's options and bounds are those of EngineOptions (neomcp/client/engine.h) and
// mean the same here; the defaults of all of them are in ClientDefaults.
struct ClientOptions {
    // ---- The protocol (EngineOptions)

    // The revisions the client speaks; empty: every built-in profile. Only the profiles that define the transport's
    // kind take part.
    std::vector<profile::ProfilePtr> profiles;
    // What the client says about itself.
    Implementation client_info;
    // The level the client asks the server's log messages at; empty: it asks none.
    std::optional<LogLevel> log_level;
    // A verdict an earlier client reached for the same server (Client::verdict): contact starts at it, without a probe.
    std::optional<ContactVerdict> prior;
    // Where the client halves of the mechanisms come from; null: builtin_client_mechanisms(). Must outlive open().
    const ClientMechanismRegistry* mechanisms = nullptr;

    std::chrono::milliseconds request_timeout = ClientDefaults::request_timeout;
    std::chrono::milliseconds max_total_timeout = ClientDefaults::max_total_timeout;
    std::chrono::milliseconds discover_probe_timeout = ClientDefaults::discover_probe_timeout;
    std::chrono::milliseconds input_handler_timeout = ClientDefaults::input_handler_timeout;
    std::size_t max_pending_requests = ClientDefaults::max_pending_requests;
    std::size_t max_queued_events = ClientDefaults::max_queued_events;
    std::size_t max_message_bytes = ClientDefaults::max_message_bytes;
    std::size_t max_nesting_depth = ClientDefaults::max_nesting_depth;
    std::size_t max_input_rounds = ClientDefaults::max_input_rounds;
    std::size_t max_input_requests_per_round = ClientDefaults::max_input_requests_per_round;
    std::size_t max_concurrent_server_requests = ClientDefaults::max_concurrent_server_requests;
    std::size_t max_restarts = ClientDefaults::max_restarts;
    std::size_t peer_schema_depth = ClientDefaults::peer_schema_depth;
    std::size_t peer_pattern_length = ClientDefaults::peer_pattern_length;
    std::size_t peer_pattern_memory = ClientDefaults::peer_pattern_memory;
    std::size_t peer_max_evaluations = ClientDefaults::peer_max_evaluations;
    std::size_t peer_patterns_per_schema = ClientDefaults::peer_patterns_per_schema;
    std::size_t max_peer_schema_compiles = ClientDefaults::max_peer_schema_compiles;

    // ---- Lists

    // The list_all_ methods follow the server's pages until one has no next cursor, and fail (never giving back a
    // partial list) when the server needs more than max_list_pages pages, or sends more than max_list_items items in
    // all (counted as sent, also those the client leaves out). Zero is an error for either. Defaults 64, 10 000. The
    // client also keeps the definitions of the tools it was given, for the checks of their results, and keeps at most
    // max_list_items of them.
    std::size_t max_list_pages = ClientDefaults::max_list_pages;
    std::size_t max_list_items = ClientDefaults::max_list_items;

    // ---- Input

    // The client's answers to a server that asks for input. The client declares a capability for each kind that has a
    // handler and for no other kind (EngineOptions::input is written from it).
    InputHandlers handlers;
    // Plain input handlers that run at once, each on a worker thread of the client; one more waits for a thread to be
    // free (its input clock runs meanwhile). Threads are started as they are needed. Coroutine handlers run on the
    // client's thread and do not count. Zero is an error. Default 4.
    std::size_t max_concurrent_input_handlers = ClientDefaults::max_concurrent_input_handlers;
    // A call whose result asks for nothing but its state is sent again at once; when it asks so again, after
    // state_retry_first_delay, doubled at each further retry up to state_retry_max_delay. A result that asks for input
    // starts the count again. Zero is an error for either, and the first must not exceed the second. Defaults 50 ms,
    // 250 ms.
    std::chrono::milliseconds state_retry_first_delay = ClientDefaults::state_retry_first_delay;
    std::chrono::milliseconds state_retry_max_delay = ClientDefaults::state_retry_max_delay;

    // ---- Change feeds (Client::listen, neomcp/client/subscription.h)

    // Subscriptions open at once; one more fails at once with too_many_subscriptions. Zero is an error. Default 16.
    std::size_t max_subscriptions = ClientDefaults::max_subscriptions;
    // A subscription whose stream was lost without the server's final result is reopened, at most this many times in
    // a row (an acknowledgement of the server starts the count again); one more loss ends it with
    // subscription_lost. Zero: never reopened. A subscription that ended with the server process (stdio) is reopened
    // at once and does not count: max_restarts bounds the restarts. Default 5.
    std::size_t max_subscription_reopens = ClientDefaults::max_subscription_reopens;
    // The wait before reopen n is drawn at random between zero and min(subscription_reopen_max_delay,
    // subscription_reopen_first_delay * 2^(n-1)) (full jitter). Zero is an error for either, and the first must not
    // exceed the second. Defaults 1 s, 60 s.
    std::chrono::milliseconds subscription_reopen_first_delay = ClientDefaults::subscription_reopen_first_delay;
    std::chrono::milliseconds subscription_reopen_max_delay = ClientDefaults::subscription_reopen_max_delay;

    // ---- Lost answers

    // The methods whose request is sent again once, with a new id and a clock of its own, when its answer was lost
    // (server_exited, stream_lost); empty: none is, and every lost answer ends its call. The names are matched exactly.
    // Add a method only when sending it twice is harmless. Default: default_resent_methods.
    std::set<std::string, std::less<>> resent_methods{default_resent_methods.begin(), default_resent_methods.end()};

    // ---- Callbacks, on the client's thread; each must return promptly, and what one throws is dropped

    // A log message of the server.
    std::function<void(const LogEvent& log)> on_log;
    // Something the application should hear of: the engine's diagnostics (an answer with an unknown id, a message that
    // is not JSON, events dropped), a handler that threw, a restarted server.
    std::function<void(const ClientDiagnostic& diagnostic)> on_diagnostic;

    // ---- Time

    // The clock and timers of the client; null: steady timers on the client's thread.
    std::shared_ptr<TimerService> timers;
};

}  // namespace neomcp
