#include "process.h"

#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace client_matrix {

namespace {

// How long the port line may take, and how long the process may take to end once asked. Reached only when something
// is broken.
constexpr auto port_bound = std::chrono::seconds(60);
constexpr auto term_grace = std::chrono::seconds(10);

void copy_to_stderr(const char* data, std::size_t size) {
    while (size > 0) {
        const ssize_t written = ::write(STDERR_FILENO, data, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return;
        data += written;
        size -= static_cast<std::size_t>(written);
    }
}

std::string error_text(int code) { return std::system_category().message(code); }

}  // namespace

ServerProcess::ServerProcess(const std::vector<std::string>& command) {
    if (command.empty()) throw std::runtime_error("no server program was given");
    int fds[2];
    if (::pipe(fds) != 0) throw std::runtime_error("no pipe: " + error_text(errno));
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
    std::vector<char*> argv;
    for (const std::string& argument : command) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    const int spawned = ::posix_spawnp(&pid_, argv.front(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(fds[1]);
    if (spawned != 0) {
        ::close(fds[0]);
        pid_ = -1;
        throw std::runtime_error("could not start " + command.front() + ": " + error_text(spawned));
    }
    stderr_ = fds[0];

    // The first line is the port; everything before its end is kept for the message of a failure.
    std::string seen;
    std::size_t newline = std::string::npos;
    const auto deadline = std::chrono::steady_clock::now() + port_bound;
    while ((newline = seen.find('\n')) == std::string::npos) {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) {
            stop();
            throw std::runtime_error("the server named no port within the bound; it wrote: " + seen);
        }
        pollfd readable{stderr_, POLLIN, 0};
        const int ready = ::poll(&readable, 1, static_cast<int>(left));
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) {
            const std::string reason = error_text(errno);
            stop();
            throw std::runtime_error("poll failed: " + reason);
        }
        if (ready == 0) continue;
        char buffer[4096];
        const ssize_t got = ::read(stderr_, buffer, sizeof buffer);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            stop();
            throw std::runtime_error("the server ended before it named its port; it wrote: " + seen);
        }
        seen.append(buffer, static_cast<std::size_t>(got));
    }
    const std::string_view line(seen.data(), newline);
    unsigned value = 0;
    const auto [end, error] = std::from_chars(line.data(), line.data() + line.size(), value);
    if (error != std::errc() || end != line.data() + line.size() || value == 0 || value > 65535) {
        stop();
        throw std::runtime_error("the server's first line is not a port; it wrote: " + seen);
    }
    port_ = static_cast<std::uint16_t>(value);
    copy_to_stderr(seen.data() + newline + 1, seen.size() - newline - 1);
    copier_ = std::thread([fd = stderr_] {
        char buffer[4096];
        for (;;) {
            const ssize_t got = ::read(fd, buffer, sizeof buffer);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) return;
            copy_to_stderr(buffer, static_cast<std::size_t>(got));
        }
    });
}

ServerProcess::~ServerProcess() { stop(); }

void ServerProcess::stop() noexcept {
    if (pid_ > 0) {
        ::kill(pid_, SIGTERM);
        bool reaped = false;
        int status = 0;
        const auto deadline = std::chrono::steady_clock::now() + term_grace;
        while (std::chrono::steady_clock::now() < deadline) {
            const pid_t ended = ::waitpid(pid_, &status, WNOHANG);
            if (ended == pid_ || (ended < 0 && errno != EINTR)) {
                reaped = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!reaped) {
            ::kill(pid_, SIGKILL);
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
        }
        pid_ = -1;
    }
    // The copy ends when the process's end of the pipe is closed, which its exit does.
    if (copier_.joinable()) copier_.join();
    if (stderr_ >= 0) {
        ::close(stderr_);
        stderr_ = -1;
    }
}

}  // namespace client_matrix
