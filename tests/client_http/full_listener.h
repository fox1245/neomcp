// A loopback listener whose queue of connections is full, for the tests of the bound on connecting.
#pragma once

#ifdef __linux__
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <vector>

namespace http_rig {

// A listener on a loopback port the system chooses that never accepts and whose queue of connections is full, so that
// a further connection is never established: its SYN is dropped and retried until the connecting side gives up.
class FullListener {
public:
    FullListener() {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) return;
        if (::listen(listener_, 0) != 0) return;
        socklen_t length = sizeof address;
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        // Connections are made until one stays unestablished: from then on the queue is full.
        for (int attempt = 0; attempt < 16 && !full_; ++attempt) {
            const int one = ::socket(AF_INET, SOCK_STREAM, 0);
            ::fcntl(one, F_SETFL, ::fcntl(one, F_GETFL) | O_NONBLOCK);
            fillers_.push_back(one);
            ::connect(one, reinterpret_cast<sockaddr*>(&address), sizeof address);
            pollfd wait{one, POLLOUT, 0};
            full_ = ::poll(&wait, 1, 200) == 0;
        }
    }
    FullListener(const FullListener&) = delete;
    FullListener& operator=(const FullListener&) = delete;
    ~FullListener() {
        for (const int one : fillers_) ::close(one);
        if (listener_ >= 0) ::close(listener_);
    }

    bool full() const noexcept { return full_; }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/mcp"; }

private:
    int listener_ = -1;
    std::uint16_t port_ = 0;
    std::vector<int> fillers_;
    bool full_ = false;
};

}  // namespace http_rig
#endif
