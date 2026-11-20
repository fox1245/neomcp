// One child process of the child-process transport, as the operating system starts, signals and collects it. The
// transport (transport.cpp) holds the protocol's side: lines, bounds, the steps of ending a process.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <asio/any_io_executor.hpp>

#ifdef _WIN32
#include <asio/windows/object_handle.hpp>
#include <asio/windows/stream_handle.hpp>
#else
#include <asio/posix/stream_descriptor.hpp>
#endif

#include "neomcp/client/transport.h"

namespace neomcp::detail {

#ifdef _WIN32
using ChildPipe = asio::windows::stream_handle;
#else
using ChildPipe = asio::posix::stream_descriptor;
#endif

using Variables = std::vector<std::pair<std::string, std::string>>;

// The environment a child gets: the variables `inherited` names that the client's environment holds, in that order,
// then `added`, where a name given in both takes the added value and keeps its place. Names compare as the platform
// compares them (exactly on POSIX, without regard to case on Windows).
Variables child_environment(std::span<const std::string> inherited, const Variables& added);

// Whether two names of variables name the same variable on this platform.
bool same_variable(std::string_view first, std::string_view second) noexcept;

#ifdef _WIN32
// UTF-8 to UTF-16 and back; empty for text that is not valid in its encoding.
std::optional<std::wstring> widen(std::string_view text);
std::optional<std::string> narrow(std::wstring_view text);
#endif

// What a process is started with: the description, checked, with its environment put together.
struct ChildLaunch {
    std::string program;  // as the application named it; also the first argument the program sees
    std::vector<std::string> arguments;
    std::optional<std::filesystem::path> working_directory;
    Variables environment;
    StderrMode stderr_mode = StderrMode::inherit;
};

// How a process ended, once collected.
struct ChildExit {
    std::optional<int> exit_code;  // it exited with this status
    std::optional<int> signal;     // POSIX: a signal ended it
};

class Child {
public:
    // POSIX: a process group can be asked to terminate before it is ended. Windows has no such step.
#ifdef _WIN32
    static constexpr bool can_terminate = false;
#else
    static constexpr bool can_terminate = true;
#endif

    // Called on the executor, posted, once the process ended: `observed` false when its end was collected elsewhere
    // (POSIX), so that it has no status and its group must not be signalled.
    using OnExit = std::function<void(bool observed)>;

    // Finds the program and starts it; a description of why when it cannot be found or started.
    static std::variant<std::unique_ptr<Child>, std::string> start(const asio::any_io_executor& executor,
                                                                    const ChildLaunch& launch, OnExit on_exit);

    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    // Ends and collects a process that was not collected, waiting for it.
    ~Child();

    std::int64_t pid() const noexcept { return pid_; }
    ChildPipe& input() noexcept { return input_; }
    ChildPipe& output() noexcept { return output_; }
    // StderrMode::capture only; null otherwise.
    ChildPipe* error() noexcept { return error_ ? &*error_ : nullptr; }

    // POSIX: SIGTERM to the group. Nothing once collected.
    void terminate() noexcept;
    // POSIX: SIGKILL to the group; Windows: the job is terminated. Nothing once collected.
    void kill() noexcept;
    // After OnExit: collects the process (POSIX: the waiting thread is joined and the process reaped) and says how it
    // ended. Once.
    ChildExit collect(bool observed);

private:
#ifdef _WIN32
    Child(std::int64_t pid, ChildPipe input, ChildPipe output, std::optional<ChildPipe> error,
          asio::windows::object_handle process, void* job);
    asio::windows::object_handle process_;
    void* job_ = nullptr;
#else
    Child(std::int64_t pid, ChildPipe input, ChildPipe output, std::optional<ChildPipe> error);
    std::thread waiter_;
#endif
    std::int64_t pid_ = 0;
    ChildPipe input_;
    ChildPipe output_;
    std::optional<ChildPipe> error_;
    bool collected_ = false;
};

// Windows: the command line that hands `arguments` to a program unchanged under the rules the C runtime and
// CommandLineToArgvW read a command line by: every argument quoted, backslashes doubled before a quote and at the end,
// a quote escaped. The first argument, the program, is quoted without escapes, as the rules read it. Built on every
// platform so that it is tested everywhere.
std::wstring windows_command_line(const std::vector<std::wstring>& arguments);

}  // namespace neomcp::detail
