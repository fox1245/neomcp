// The client engine paired in process with the server's session, without sockets: what the engine writes goes to the
// session and what the session writes goes back to the engine. The server's handlers run on its own threads, so what
// the session writes is queued and a test waits for it with a bound; the engine is touched on the test's thread only.
#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "engine_rig.h"
#include "neomcp/server.h"

namespace client_core_test {

inline constexpr auto pair_wait_bound = std::chrono::seconds(20);

// What the session wrote, as the transport would hand it on.
struct Written {
    std::optional<neomcp::UnitId> unit;
    std::string line;
    bool last = false;
    bool end = false;
};

class Pair {
public:
    using Populate = std::function<void(neomcp::Server&)>;

    Pair(neomcp::EngineOptions options, neomcp::profile::Transport transport, neomcp::ServerOptions served,
         const Populate& populate)
        : server_(std::move(served)), rig_(std::move(options), transport), transport_(transport) {
        populate(server_);
        const auto lines = [this](std::string line) { push(Written{std::nullopt, line, false, false}); };
        const auto outputs = [this](neomcp::Output output) {
            push(Written{output.unit, output.line, output.last, output.kind == neomcp::OutputKind::end});
        };
        auto connected = transport == neomcp::profile::Transport::stdio ? server_.connect(lines)
                                                                        : server_.connect_tagged(transport, outputs);
        if (auto* error = std::get_if<neomcp::CreateError>(&connected)) throw std::runtime_error(error->message);
        session_ = std::move(std::get<std::unique_ptr<neomcp::Session>>(connected));
    }

    Rig& rig() { return rig_; }

    // Called before what the engine wrote is handed on, each time the pair goes round: a test that answers the
    // engine's events (an input dispatcher) does it here, so that its answers go out in the same turn.
    std::function<void()> on_turn;

    // Hands what the engine wrote to the session and what the session wrote to the engine, until `done` holds or the
    // bound passes.
    bool pump(const std::function<bool()>& done) {
        const auto deadline = std::chrono::steady_clock::now() + pair_wait_bound;
        while (true) {
            if (on_turn) on_turn();
            forward();
            if (done()) return true;
            Written written;
            {
                std::unique_lock lock(mutex_);
                if (!ready_.wait_until(lock, deadline, [&] { return !queue_.empty(); })) return done();
                written = std::move(queue_.front());
                queue_.pop_front();
            }
            deliver(written);
        }
    }

    void open() {
        rig_.engine->open();
        const bool reached = pump([&] {
            const auto phase = rig_.engine->contact().phase;
            return phase == neomcp::ContactPhase::open || phase == neomcp::ContactPhase::failed;
        });
        if (!reached || rig_.engine->contact().phase != neomcp::ContactPhase::open) {
            throw std::runtime_error("contact did not open");
        }
        // The ready notification of a handshake reaches the server before anything else does.
        pump([] { return true; });
    }

    neomcp::RequestEnded call(neomcp::OutgoingCall outgoing) {
        const auto ref = rig_.send(std::move(outgoing));
        std::optional<neomcp::RequestEnded> ended;
        pump([&] { return (ended = rig_.ended(ref)).has_value(); });
        if (!ended) throw std::runtime_error("the call did not end in time");
        return *ended;
    }

    neomcp::RequestEnded call(std::string method, json params, json input_schema = nullptr) {
        neomcp::OutgoingCall outgoing;
        outgoing.message.method = std::move(method);
        outgoing.message.params = std::move(params);
        outgoing.input_schema = std::move(input_schema);
        return call(std::move(outgoing));
    }

    // The result of a call that must be answered with one.
    json result(std::string method, json params, json input_schema = nullptr) {
        const auto ended = call(method, std::move(params), std::move(input_schema));
        if (const auto* answered = std::get_if<neomcp::Answered>(&ended.outcome)) return answered->result;
        if (const auto* error = std::get_if<neomcp::ErrorAnswered>(&ended.outcome)) {
            throw std::runtime_error(method + " was answered with an error: " + error->error.message);
        }
        const auto& failure = std::get<neomcp::CallFailure>(ended.outcome);
        throw std::runtime_error(method + " failed: " + failure.message);
    }

    const neomcp::profile::Profile& profile() const { return *rig_.engine->contact().profile; }

private:
    void push(Written written) {
        {
            std::lock_guard lock(mutex_);
            queue_.push_back(std::move(written));
        }
        ready_.notify_all();
    }

    void forward() {
        for (; forwarded_ < rig_.out.size(); ++forwarded_) {
            const neomcp::Outgoing outgoing = rig_.out[forwarded_];
            switch (outgoing.kind) {
                case neomcp::OutgoingKind::request:
                case neomcp::OutgoingKind::notification:
                case neomcp::OutgoingKind::response:
                    if (transport_ == neomcp::profile::Transport::stdio) {
                        session_->receive(outgoing.line);
                    } else {
                        std::vector<neomcp::HeaderField> fields;
                        for (const auto& header : outgoing.headers) fields.push_back({header.name, header.value});
                        session_->receive(outgoing.unit, outgoing.line, fields);
                    }
                    break;
                case neomcp::OutgoingKind::end_unit:
                    if (transport_ != neomcp::profile::Transport::stdio) session_->abandon(outgoing.unit);
                    break;
                default: break;
            }
        }
    }

    void deliver(const Written& written) {
        if (transport_ == neomcp::profile::Transport::stdio) {
            rig_.engine->received(neomcp::connection_unit, written.line);
            return;
        }
        const auto unit = written.unit.value_or(neomcp::connection_unit);
        if (!written.end) rig_.engine->received(unit, written.line);
        if (written.end || written.last) rig_.engine->stream_event(unit, neomcp::StreamEnded{});
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Written> queue_;
    neomcp::Server server_;
    Rig rig_;
    neomcp::profile::Transport transport_;
    std::size_t forwarded_ = 0;
    std::unique_ptr<neomcp::Session> session_;
};

}  // namespace client_core_test
