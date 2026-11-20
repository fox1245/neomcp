// A transport inside the process: what the client writes goes to a peer object, and what the peer sends goes back to
// the client, without sockets, pipes or processes. For tests, and for an application that runs its server in the same
// process (neomcp/client/in_process.h connects a client to a neomcp::Server this way).
//
// A peer stands for the server's side of one transport kind:
// - stdio: every message is on `connection_unit`. A `restart` output asks the peer to stand for a new server process;
//   it answers with MemoryLink::restarted(). The end of a process is the event ProcessExited on `connection_unit`.
// - HTTP: each request, notification and response of the client opens its unit, numbered by the engine; the peer
//   answers on that unit and ends it with StreamEnded (or loses it with StreamLost). What the server sends outside any
//   request goes on `connection_unit`, once the client opened that stream (`open_connection_stream`).
//
// Threads. The peer's methods are called on the client's thread and must not block it for long. The link may be used
// from any thread, at any time: what it is given is delivered to the client on the client's thread, in the order the
// link was given it, and dropped once the transport is closed.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "neomcp/client/engine.h"
#include "neomcp/client/transport.h"
#include "neomcp/profile.h"

namespace neomcp {

namespace detail {
class MemoryLinkState;
}  // namespace detail

// How a peer speaks to its client. Cheap to copy; copies speak for the same transport.
class MemoryLink {
public:
    // A message of the server on a unit (one line, without a terminator).
    void message(UnitId unit, std::string message) const;
    // Something about a unit's exchange other than a message (see StreamEvent).
    void event(UnitId unit, StreamEvent event) const;
    // stdio: a new server process stands in for the old one (the answer to a `restart` output).
    void restarted() const;
    // Whether the transport still delivers what the link is given.
    bool open() const noexcept;

private:
    friend class detail::MemoryLinkState;
    explicit MemoryLink(std::shared_ptr<detail::MemoryLinkState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::MemoryLinkState> state_;
};

// The server's side of an in-memory transport.
class MemoryPeer {
public:
    virtual ~MemoryPeer() = default;

    // The transport starts: what the peer sends goes through `link`. Returns why the peer cannot start, or nothing.
    virtual std::optional<std::string> attach(MemoryLink link) = 0;

    // One output of the client's engine, of any kind but OutgoingKind::close.
    virtual void receive(Outgoing outgoing) = 0;

    // The transport closes (`cause` says why); nothing more is received, and the link delivers nothing more.
    virtual void detach(CloseCause cause) = 0;
};

// A transport of `kind` whose server side is `peer`. The transport keeps the peer alive until it is destroyed.
std::unique_ptr<Transport> make_memory_transport(profile::Transport kind, std::shared_ptr<MemoryPeer> peer);

}  // namespace neomcp
