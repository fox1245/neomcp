#include "stdio/fd_streams.h"

#include <cerrno>
#include <cstddef>
#include <memory>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <limits>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace neomcp::detail {

#ifdef _WIN32

FdReader::FdReader(int fd) : fd_(fd) {}
FdReader::~FdReader() = default;

std::size_t FdReader::read(char* buffer, std::size_t capacity) {
    constexpr std::size_t most = std::numeric_limits<int>::max();
    for (;;) {
        if (interrupted_.load()) return 0;
        const int count = _read(fd_, buffer, static_cast<unsigned int>(capacity < most ? capacity : most));
        if (count > 0) return static_cast<std::size_t>(count);
        if (count < 0 && errno == EINTR) continue;
        return 0;
    }
}

void FdReader::interrupt() noexcept { interrupted_.store(true); }

bool FdWriter::write(std::string_view bytes) {
    constexpr std::size_t most = std::numeric_limits<int>::max();
    while (!bytes.empty()) {
        const std::size_t part = bytes.size() < most ? bytes.size() : most;
        const int count = _write(fd_, bytes.data(), static_cast<unsigned int>(part));
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (count == 0) return false;
        bytes.remove_prefix(static_cast<std::size_t>(count));
    }
    return true;
}

BinaryMode::BinaryMode(int fd) : fd_(fd), previous_(_setmode(fd, _O_BINARY)) {}
BinaryMode::~BinaryMode() {
    if (previous_ != -1) _setmode(fd_, previous_);
}

struct SigpipeGuard::Saved {};
SigpipeGuard::SigpipeGuard(bool) {}
SigpipeGuard::~SigpipeGuard() = default;

#else

FdReader::FdReader(int fd) : fd_(fd) {
    int ends[2] = {-1, -1};
    if (::pipe(ends) != 0) return;  // without it the read cannot be interrupted, and still works
    for (const int end : ends) {
        ::fcntl(end, F_SETFD, ::fcntl(end, F_GETFD) | FD_CLOEXEC);
        ::fcntl(end, F_SETFL, ::fcntl(end, F_GETFL) | O_NONBLOCK);
    }
    wake_read_ = ends[0];
    wake_write_ = ends[1];
}

FdReader::~FdReader() {
    if (wake_read_ >= 0) ::close(wake_read_);
    if (wake_write_ >= 0) ::close(wake_write_);
}

std::size_t FdReader::read(char* buffer, std::size_t capacity) {
    for (;;) {
        if (wake_read_ >= 0) {
            pollfd waits[2] = {{fd_, POLLIN, 0}, {wake_read_, POLLIN, 0}};
            if (::poll(waits, 2, -1) < 0) {
                if (errno == EINTR) continue;
                return 0;
            }
            if ((waits[1].revents & POLLIN) != 0) return 0;  // interrupted, for good: the pipe is never drained
            if (waits[0].revents == 0) continue;
        }
        const ssize_t count = ::read(fd_, buffer, capacity);
        if (count > 0) return static_cast<std::size_t>(count);
        if (count < 0 && (errno == EINTR || (errno == EAGAIN && wake_read_ >= 0))) continue;
        return 0;
    }
}

void FdReader::interrupt() noexcept {
    if (wake_write_ < 0) return;
    const char byte = 1;
    ssize_t written;
    do {
        written = ::write(wake_write_, &byte, 1);
    } while (written < 0 && errno == EINTR);
    // A full pipe means an interrupt is pending already; any other failure leaves the read to end with the input.
}

bool FdWriter::write(std::string_view bytes) {
    while (!bytes.empty()) {
        const ssize_t count = ::write(fd_, bytes.data(), bytes.size());
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (count == 0) return false;
        bytes.remove_prefix(static_cast<std::size_t>(count));
    }
    return true;
}

BinaryMode::BinaryMode(int) {}
BinaryMode::~BinaryMode() = default;

struct SigpipeGuard::Saved {
    struct sigaction action;
};

SigpipeGuard::SigpipeGuard(bool enabled) {
    if (!enabled) return;
    struct sigaction ignore = {};
    ignore.sa_handler = SIG_IGN;
    ::sigemptyset(&ignore.sa_mask);
    auto saved = std::make_unique<Saved>();
    if (::sigaction(SIGPIPE, &ignore, &saved->action) == 0) saved_ = std::move(saved);
}

SigpipeGuard::~SigpipeGuard() {
    if (saved_) ::sigaction(SIGPIPE, &saved_->action, nullptr);
}

#endif

}  // namespace neomcp::detail
