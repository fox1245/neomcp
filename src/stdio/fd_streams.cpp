#include "stdio/fd_streams.h"

#include <cerrno>
#include <cstddef>
#include <mutex>

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

// There is no private pipe here: a read is ended by the flag that interrupt() sets, so both forms make the same reader.
FdReader::FdReader(int fd, bool) : fd_(fd) {}
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

SigpipeGuard::SigpipeGuard(bool) {}
SigpipeGuard::~SigpipeGuard() = default;

#else

FdReader::FdReader(int fd, bool interruptible) : fd_(fd) {
    if (!interruptible) return;
    int ends[2] = {-1, -1};
#ifdef __linux__
    // One step: the descriptors are close-on-exec and non-blocking from the moment they exist.
    if (::pipe2(ends, O_CLOEXEC | O_NONBLOCK) != 0) return;  // without it the read cannot be interrupted, and still works
#else
    // Where there is no pipe2 (macOS), the flags are set in a second step. A child that another thread starts between
    // the two calls inherits both ends of this pipe. That costs the child two open descriptors and nothing else: the
    // pipe is only ever written by interrupt(), and the child has no reason to read it.
    if (::pipe(ends) != 0) return;  // without it the read cannot be interrupted, and still works
    for (const int end : ends) {
        ::fcntl(end, F_SETFD, ::fcntl(end, F_GETFD) | FD_CLOEXEC);
        ::fcntl(end, F_SETFL, ::fcntl(end, F_GETFL) | O_NONBLOCK);
    }
#endif
    wake_read_ = ends[0];
    wake_write_ = ends[1];
}

FdReader::~FdReader() {
    if (wake_read_ >= 0) ::close(wake_read_);
    if (wake_write_ >= 0) ::close(wake_write_);
}

std::size_t FdReader::read(char* buffer, std::size_t capacity) {
    for (;;) {
        // Every wait is a poll, with or without the private pipe: a descriptor that is in non-blocking mode (its
        // owner may have left it so) answers a read with EAGAIN when nothing has arrived, and that is a reason to
        // wait, not the end of the input.
        pollfd waits[2] = {{fd_, POLLIN, 0}, {wake_read_, POLLIN, 0}};
        const nfds_t watched = wake_read_ >= 0 ? 2 : 1;
        if (::poll(waits, watched, -1) < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        if (watched == 2 && (waits[1].revents & POLLIN) != 0) return 0;  // interrupted, for good: the pipe is never drained
        if (waits[0].revents == 0) continue;
        const ssize_t count = ::read(fd_, buffer, capacity);
        if (count > 0) return static_cast<std::size_t>(count);
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
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

namespace {

// The guards that are alive, and the disposition from before the first of them.
std::mutex sigpipe_mutex;
std::size_t sigpipe_guards = 0;
struct sigaction sigpipe_original;

}  // namespace

SigpipeGuard::SigpipeGuard(bool enabled) {
    if (!enabled) return;
    const std::lock_guard<std::mutex> lock(sigpipe_mutex);
    if (sigpipe_guards == 0) {
        struct sigaction ignore = {};
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);  // unqualified: some platforms define it as a macro
        if (::sigaction(SIGPIPE, &ignore, &sigpipe_original) != 0) return;
    }
    ++sigpipe_guards;
    held_ = true;
}

SigpipeGuard::~SigpipeGuard() {
    if (!held_) return;
    const std::lock_guard<std::mutex> lock(sigpipe_mutex);
    if (--sigpipe_guards == 0) ::sigaction(SIGPIPE, &sigpipe_original, nullptr);
}

#endif

}  // namespace neomcp::detail
