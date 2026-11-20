// Opens a client over the in-memory transport with a peer that refuses to start, and links only neomcp::client. It
// exits 0 when opening failed as the transport said.
#include <memory>
#include <optional>
#include <string>

#include "neomcp/client.h"
#include "neomcp/client/memory_transport.h"

namespace {

class RefusingPeer final : public neomcp::MemoryPeer {
public:
    std::optional<std::string> attach(neomcp::MemoryLink) override { return std::string("no server here"); }
    void receive(neomcp::Outgoing) override {}
    void detach(neomcp::CloseCause) override {}
};

}  // namespace

int main() {
    auto transport = neomcp::make_memory_transport(neomcp::profile::Transport::stdio, std::make_shared<RefusingPeer>());
    auto opened = neomcp::Client::open(neomcp::ClientOptions{}, std::move(transport));
    if (opened) return 1;
    return opened.error().kind == neomcp::ClientErrorKind::transport ? 0 : 1;
}
