// A rig for the tests of the client engine without a server: it collects what the engine writes and the events it
// queues, and writes the answers a test scripts.
#pragma once

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "neomcp/client/engine.h"
#include "neomcp/jsonrpc.h"
#include "neomcp/profile.h"

namespace client_core_test {

using neomcp::json;

// Options that enable exactly these revisions.
inline neomcp::EngineOptions only(std::initializer_list<std::string_view> revisions) {
    neomcp::EngineOptions options;
    for (const auto revision : revisions) options.profiles.push_back(neomcp::profile::builtin(revision));
    options.client_info.name = "engine-test";
    options.client_info.version = "1.0.0";
    return options;
}

class Rig {
public:
    explicit Rig(neomcp::EngineOptions options,
                 neomcp::profile::Transport transport = neomcp::profile::Transport::stdio)
        : transport_(transport) {
        auto made = neomcp::ClientEngine::create(std::move(options), transport, [this](neomcp::Outgoing outgoing) {
            if (on_output) on_output(outgoing);
            out.push_back(std::move(outgoing));
        });
        if (auto* error = std::get_if<neomcp::ClientMechanismsError>(&made)) throw std::runtime_error(error->message);
        engine = std::move(std::get<std::unique_ptr<neomcp::ClientEngine>>(made));
    }

    // Moves every queued event into `log` and returns it.
    std::vector<neomcp::EngineEvent>& drain() {
        while (auto event = engine->next_event()) log.push_back(std::move(*event));
        return log;
    }

    template <class T>
    std::vector<T> all() {
        drain();
        std::vector<T> found;
        for (const auto& event : log) {
            if (const auto* one = std::get_if<T>(&event)) found.push_back(*one);
        }
        return found;
    }

    // The end of a call, when it ended.
    std::optional<neomcp::RequestEnded> ended(neomcp::RequestRef ref) {
        for (const auto& event : all<neomcp::RequestEnded>()) {
            if (event.request == ref) return event;
        }
        return std::nullopt;
    }

    std::vector<neomcp::Diagnostic> diagnostics(neomcp::DiagnosticKind kind) {
        std::vector<neomcp::Diagnostic> found;
        for (const auto& diagnostic : all<neomcp::Diagnostic>()) {
            if (diagnostic.kind == kind) found.push_back(diagnostic);
        }
        return found;
    }

    // The outputs of a kind, in order.
    std::vector<neomcp::Outgoing> outputs(neomcp::OutgoingKind kind) const {
        std::vector<neomcp::Outgoing> found;
        for (const auto& outgoing : out) {
            if (outgoing.kind == kind) found.push_back(outgoing);
        }
        return found;
    }

    // The requests written, parsed, in order.
    std::vector<json> requests() const {
        std::vector<json> found;
        for (const auto& outgoing : outputs(neomcp::OutgoingKind::request)) found.push_back(json::parse(outgoing.line));
        return found;
    }

    // The last request of `method`, with the unit it went out on.
    std::optional<std::pair<json, neomcp::UnitId>> last(std::string_view method) const {
        for (auto it = out.rbegin(); it != out.rend(); ++it) {
            if (it->kind != neomcp::OutgoingKind::request) continue;
            json message = json::parse(it->line);
            if (message["method"] == std::string(method)) return std::make_pair(message, it->unit);
        }
        return std::nullopt;
    }

    std::uint64_t last_id(std::string_view method) const {
        auto found = last(method);
        if (!found) throw std::runtime_error("no request " + std::string(method) + " was written");
        return found->first["id"].get<std::uint64_t>();
    }

    neomcp::UnitId last_unit(std::string_view method) const {
        auto found = last(method);
        if (!found) throw std::runtime_error("no request " + std::string(method) + " was written");
        return found->second;
    }

    // Writes an answer to the last request of `method` on the unit it went out on (stdio: the connection).
    neomcp::Reception answer(std::string_view method, json result) {
        const auto unit = answer_unit(method);
        return engine->received(unit, neomcp::serialize(neomcp::make_result(neomcp::RequestId(last_id(method)),
                                                                          std::move(result))));
    }
    neomcp::Reception refuse(std::string_view method, int code, std::string message, json data = nullptr) {
        const auto unit = answer_unit(method);
        return engine->received(unit, neomcp::serialize(neomcp::make_error(neomcp::RequestId(last_id(method)), code,
                                                                         std::move(message), std::move(data))));
    }

    neomcp::RequestRef send(std::string method, json params = json::object()) {
        neomcp::OutgoingCall call;
        call.message.method = std::move(method);
        call.message.params = std::move(params);
        return send(std::move(call));
    }
    neomcp::RequestRef send(neomcp::OutgoingCall call) {
        auto sent = engine->send(std::move(call));
        if (auto* failure = std::get_if<neomcp::CallFailure>(&sent)) {
            throw std::runtime_error("send failed: " + failure->message);
        }
        return std::get<neomcp::RequestRef>(sent);
    }

    // The deadline of a call's clock as last changed, when it has one.
    std::optional<neomcp::DeadlineChange> clock(neomcp::DeadlineOwner owner, neomcp::DeadlineKind kind) {
        std::optional<neomcp::DeadlineChange> found;
        for (const auto& change : all<neomcp::DeadlineChange>()) {
            if (change.deadline.owner == owner && change.deadline.kind == kind) found = change;
        }
        return found;
    }

    neomcp::profile::Transport transport() const noexcept { return transport_; }

    std::vector<neomcp::Outgoing> out;
    std::vector<neomcp::EngineEvent> log;
    std::function<void(const neomcp::Outgoing&)> on_output;
    std::unique_ptr<neomcp::ClientEngine> engine;

private:
    neomcp::UnitId answer_unit(std::string_view method) const {
        return transport_ == neomcp::profile::Transport::streamable_http ? last_unit(method) : neomcp::connection_unit;
    }
    neomcp::profile::Transport transport_ = neomcp::profile::Transport::stdio;
};

// A discovery result that names `versions`.
inline json discover_result(std::initializer_list<std::string_view> versions) {
    json list = json::array();
    for (const auto version : versions) list.push_back(std::string(version));
    return json{{"supportedVersions", list}, {"capabilities", json::object()}, {"ttlMs", 0},
                {"cacheScope", "private"}, {"resultType", "complete"}};
}

// A handshake result at `revision`.
inline json initialize_result(std::string_view revision) {
    return json{{"protocolVersion", std::string(revision)},
                {"capabilities", json::object()},
                {"serverInfo", json{{"name", "scripted"}, {"version", "1"}}}};
}

}  // namespace client_core_test
