// ByteReader and ByteWriter over file descriptors: POSIX descriptors, or the C runtime's descriptors on Windows.
#pragma once

#include <atomic>
#include <cstddef>
#include <string_view>

#include "neomcp/stdio.h"

namespace neomcp::detail {

// Reads from a descriptor, retrying when a signal interrupts the call. On POSIX the read waits in poll() on the
// descriptor and on a private pipe that interrupt() writes to, so that another thread can end the wait. On Windows
// there is no portable way to end a blocked read, and interrupt() only makes the next read return 0.
class FdReader final : public ByteReader {
public:
    // `interruptible` false makes a reader without the private pipe, which is also what a reader is when the pipe could
    // not be created: its wait cannot be ended by interrupt(), and it still waits for input without ending early.
    explicit FdReader(int fd, bool interruptible = true);
    ~FdReader() override;
    FdReader(const FdReader&) = delete;
    FdReader& operator=(const FdReader&) = delete;

    std::size_t read(char* buffer, std::size_t capacity) override;
    void interrupt() noexcept override;

private:
    int fd_;
#ifdef _WIN32
    std::atomic<bool> interrupted_{false};  // set by interrupt(), read by read()
#else
    int wake_read_ = -1;  // the pipe interrupt() writes to
    int wake_write_ = -1;
#endif
};

// Writes to a descriptor until every byte is written, retrying when a signal interrupts the call.
class FdWriter final : public ByteWriter {
public:
    explicit FdWriter(int fd) : fd_(fd) {}
    bool write(std::string_view bytes) override;

private:
    int fd_;
};

// Windows: puts a descriptor in binary mode for the lifetime of the object and restores its previous mode. Elsewhere
// it does nothing.
class BinaryMode {
public:
    explicit BinaryMode(int fd);
    ~BinaryMode();
    BinaryMode(const BinaryMode&) = delete;
    BinaryMode& operator=(const BinaryMode&) = delete;

private:
#ifdef _WIN32
    int fd_;
    int previous_ = -1;
#endif
};

// POSIX: ignores SIGPIPE for the lifetime of the object when `enabled`. Guards may overlap in any order: the
// disposition from before the first one is restored when the last one is gone. Elsewhere it does nothing.
class SigpipeGuard {
public:
    explicit SigpipeGuard(bool enabled);
    ~SigpipeGuard();
    SigpipeGuard(const SigpipeGuard&) = delete;
    SigpipeGuard& operator=(const SigpipeGuard&) = delete;

private:
    bool held_ = false;  // this guard counts among those that keep SIGPIPE ignored
};

}  // namespace neomcp::detail
