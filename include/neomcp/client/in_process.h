// A client connected to a neomcp::Server in the same process, through the in-memory transport (target
// neomcp::client_in_process, which links the server's library).
//
//   neomcp::Server server({.name = "calc", .version = "1.0.0"});
//   server.tool<AddArgs>("add", "Add two integers", [](const AddArgs& args) { return args.a + args.b; });
//   auto peer = neomcp::InProcessServer::create(server);
//   auto client = neomcp::Client::open({}, peer->transport());
//
// The server's session sees exactly what it would see over the transport: lines on stdio, and on HTTP one unit per
// exchange with the header fields the client writes. Each server process the transport stands for is one Session of
// the server (Server::connect, or Server::connect_tagged for HTTP); a restart closes it and connects a new one.
#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include "neomcp/client/memory_transport.h"
#include "neomcp/profile.h"
#include "neomcp/server.h"

namespace neomcp {

namespace detail {
class InProcessServerCore;
}  // namespace detail

class InProcessServer final : public MemoryPeer, public std::enable_shared_from_this<InProcessServer> {
public:
    // A peer for one client over a transport of `kind`. The server must outlive the peer, and its registrations must
    // be complete (the first session it connects closes them).
    static std::shared_ptr<InProcessServer> create(Server& server, profile::Transport kind = profile::Transport::stdio);

    InProcessServer(const InProcessServer&) = delete;
    InProcessServer& operator=(const InProcessServer&) = delete;
    ~InProcessServer() override;

    // The transport to open a client with; once. A second call throws std::logic_error.
    std::unique_ptr<Transport> transport();

    // stdio: the server process ends, as one that exits on its own: its session closes and the client is told
    // (ProcessExited with `status`). What the session would write as it closes (the graceful end of its
    // subscriptions) does not reach the client, as a process that is gone writes nothing. The client starts a new one at its next request. Callable from any thread;
    // ignored while no process runs, and on HTTP.
    void end_process(std::optional<int> status = 0);

    // Server processes started so far: 1 once the client opened, one more for each restart.
    std::size_t processes() const noexcept;

    // MemoryPeer, called by the transport on the client's thread.
    std::optional<std::string> attach(MemoryLink link) override;
    void receive(Outgoing outgoing) override;
    void detach(CloseCause cause) override;

private:
    InProcessServer(Server& server, profile::Transport kind);
    std::unique_ptr<detail::InProcessServerCore> core_;
};

}  // namespace neomcp
