// An in-memory client for tests: it feeds a Session lines and collects what the session writes.
//
// Every wait is on a condition variable with a caller-given timeout; nothing polls or sleeps. What the session
// writes is sorted as it arrives: responses with an id go to the request that waits for that id, notifications to
// next_notification(), requests to the client to next_request(), and everything else (batch answers, errors with id
// null) to next_other(). transcript() keeps every line in the order it was written.
#pragma once

#include <chrono>
#include <concepts>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "neomcp/json.h"
#include "neomcp/jsonrpc.h"
#include "neomcp/profile.h"
#include "neomcp/session.h"
#include "neomcp/tool_host.h"

namespace neomcp::testing {

// Something that opens sessions itself, as neomcp::Server::connect does: it builds the session's options and gives
// the session its tools.
template <class Factory>
concept SessionFactory = requires(Factory& factory, Sink sink) {
    { factory.connect(std::move(sink)) } -> std::same_as<std::variant<std::unique_ptr<Session>, CreateError>>;
};

class Peer {
public:
    using Timeout = std::chrono::milliseconds;

    // Creates the session; throws std::runtime_error with the session's error message when it cannot be created.
    explicit Peer(ToolHost& host, SessionOptions options = {});
    // Opens the session through the factory (for example `neomcp::testing::Peer peer(server);`), which must outlive
    // the peer's use of it as its contract says. Throws like the constructor above.
    template <SessionFactory Factory>
    explicit Peer(Factory& factory)
        : Peer(Connector{[&factory](Sink sink) { return factory.connect(std::move(sink)); }}) {}
    ~Peer();
    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;

    // Sends a request and returns the first response with its id that arrives; empty after the timeout.
    std::optional<json> request(const json& message, Timeout timeout);
    // Waits for the next response with this id, for a request sent earlier; empty after the timeout.
    std::optional<json> await_response(const RequestId& id, Timeout timeout);
    // Sends a message without waiting for anything.
    void notify(const json& message);
    // Sends one line as it is.
    void send(std::string_view line);

    std::optional<json> next_notification(Timeout timeout);
    // The next request the session sent to the client (it has a method and an id).
    std::optional<json> next_request(Timeout timeout);
    std::optional<json> next_other(Timeout timeout);
    // Every message the session wrote so far, in order.
    std::vector<json> transcript() const;

    void close();
    Session& session() noexcept { return *session_; }

    // ---- Building messages from a profile, so that tests need not write them per revision.

    // The initialize request of a client that asks for the profile's revision.
    static json initialize_request(const profile::Profile& profile, const RequestId& id,
                                   json capabilities = json::object());
    // Sends initialize_request, waits for its response and then sends notifications/initialized. Returns the
    // response; empty after the timeout.
    std::optional<json> initialize(const profile::Profile& profile, Timeout timeout);
    // The `_meta` a request names the profile's revision with: every key the profile's RequestMetaObject requires
    // (protocol version and client capabilities). Throws std::logic_error when the profile has no RequestMetaObject
    // or requires a key this helper cannot fill.
    static json request_meta(const profile::Profile& profile, json capabilities = json::object());
    // A request for the profile: with request_meta in params._meta when the profile has a RequestMetaObject,
    // as it is otherwise.
    static json make_request(const profile::Profile& profile, const RequestId& id, std::string method,
                             json params = json::object());

private:
    struct Connector {
        std::function<std::variant<std::unique_ptr<Session>, CreateError>(Sink)> open;
    };
    explicit Peer(Connector connector);

    void deliver(std::string line);

    mutable std::mutex mutex_;
    std::condition_variable arrived_;
    std::map<std::string, std::deque<json>> responses_;  // by the id's serialised form
    std::deque<json> notifications_;
    std::deque<json> requests_;
    std::deque<json> others_;
    std::vector<json> transcript_;

    std::mutex send_mutex_;  // receive() is called by one thread at a time
    std::unique_ptr<Session> session_;
};

}  // namespace neomcp::testing
