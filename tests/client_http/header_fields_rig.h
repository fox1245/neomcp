// Shared by the tests of the header fields of the application's own: what supplies them per request and records what it
// was asked, a peer that opens a session, and the name of a request kind.
#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "neomcp/client/engine.h"
#include "neomcp/client/transport.h"
#include "neomcp/profile.h"
#include "scripted_http_server.h"

namespace fields_rig {

inline std::string kind_name(neomcp::HttpRequestKind kind) {
    switch (kind) {
        case neomcp::HttpRequestKind::message: return "message";
        case neomcp::HttpRequestKind::connection_stream: return "connection_stream";
        case neomcp::HttpRequestKind::stream_resumption: return "stream_resumption";
        case neomcp::HttpRequestKind::end_session: return "end_session";
    }
    return "unknown";
}

// The field a Supplier writes by default: the name of the kind it was asked for.
inline constexpr const char* kind_field = "X-App-Kind";
inline constexpr const char* fixed_field = "X-App-Fixed";

// The callback of an endpoint: records every kind it is asked for, and answers by the test's script, which is told the
// number of the call (from zero). Without a script it answers with the kind's name under kind_field. It runs on the
// client's thread; the records are read from the test's.
class Supplier {
public:
    using Script = std::function<std::vector<neomcp::HttpHeader>(neomcp::HttpRequestKind, std::size_t call)>;

    Supplier() = default;
    explicit Supplier(Script script) : script_(std::move(script)) {}
    Supplier(const Supplier&) = delete;
    Supplier& operator=(const Supplier&) = delete;

    neomcp::HttpFieldsCallback callback() {
        return [this](neomcp::HttpRequestKind kind) {
            std::size_t call = 0;
            {
                std::lock_guard lock(mutex_);
                call = kinds_.size();
                kinds_.push_back(kind);
            }
            if (script_) return script_(kind, call);
            return std::vector<neomcp::HttpHeader>{{kind_field, kind_name(kind)}};
        };
    }

    std::vector<neomcp::HttpRequestKind> kinds() const {
        std::lock_guard lock(mutex_);
        return kinds_;
    }
    std::size_t calls() const {
        std::lock_guard lock(mutex_);
        return kinds_.size();
    }

private:
    Script script_;
    mutable std::mutex mutex_;
    std::vector<neomcp::HttpRequestKind> kinds_;
};

// A peer of 2025-11-25 that opens a session named "session-1" at the handshake; the test's handler answers what it
// wants first, and what it leaves is answered as a server does: a GET with 405, a DELETE with 200, notifications and
// answers with 202, any other request with an empty result.
class SessionPeer {
public:
    using Handler = std::function<std::optional<http_test::Reply>(const http_test::Request&)>;

    explicit SessionPeer(Handler handler = {})
        : handler_(std::move(handler)), server_([this](const http_test::Request& request) { return reply(request); }) {}

    std::string url() const { return server_.url(); }
    http_test::ScriptedHttpServer& server() { return server_; }

private:
    http_test::Reply reply(const http_test::Request& request) {
        using http_test::json;
        if (request.method == "POST" && request.rpc_method() == "initialize") {
            json result{{"protocolVersion", "2025-11-25"},
                        {"capabilities", json::object()},
                        {"serverInfo", {{"name", "scripted"}, {"version", "1"}}}};
            return http_test::json_reply(200, http_test::result_for(request, std::move(result)),
                                         {{"MCP-Session-Id", "session-1"}});
        }
        if (handler_) {
            if (auto chosen = handler_(request)) return std::move(*chosen);
        }
        if (request.method == "GET") return http_test::empty_reply(405);
        if (request.method == "DELETE") return http_test::empty_reply(200);
        if (request.rpc_id().is_null()) return http_test::empty_reply(202);
        return http_test::json_reply(200, http_test::result_for(request, json::object()));
    }

    Handler handler_;
    http_test::ScriptedHttpServer server_;  // last: it goes first, so that no request reaches the members after it
};

inline neomcp::profile::ProfilePtr legacy_profile() { return neomcp::profile::builtin("2025-11-25"); }

}  // namespace fields_rig
