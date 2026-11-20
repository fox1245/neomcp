// A server program started for a test (POSIX only): it is started with its standard input from /dev/null, reports the
// port it listens on as the first line of its standard error, and runs until it is asked to end. The rest of its
// standard error is copied to the test's own, so that what the server says is in the test's output.
#pragma once

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>

namespace client_matrix {

class ServerProcess {
public:
    // Starts `command` (the program, found through PATH when it names no directory, and its arguments) and waits for
    // the port line. Throws std::runtime_error, with what the process wrote, when it cannot start or ends first.
    explicit ServerProcess(const std::vector<std::string>& command);
    // Asks the process to terminate (SIGTERM), ends it (SIGKILL) when it is still running after a grace, and reaps it.
    ~ServerProcess();
    ServerProcess(const ServerProcess&) = delete;
    ServerProcess& operator=(const ServerProcess&) = delete;

    std::uint16_t port() const noexcept { return port_; }

private:
    void stop() noexcept;

    pid_t pid_ = -1;
    int stderr_ = -1;
    std::uint16_t port_ = 0;
    std::thread copier_;
};

}  // namespace client_matrix
