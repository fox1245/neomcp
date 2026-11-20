// A child process on POSIX: posix_spawn into a process group of its own, pipes for its standard streams, and one
// thread that waits for its end.
#ifndef _WIN32

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <system_error>

#include <asio/post.hpp>

#include "client_stdio/child.h"

namespace neomcp::detail {

namespace {

constexpr std::string_view path_variable = "PATH";
constexpr char path_separator = ':';
constexpr char directory_separator = '/';
constexpr const char* null_device = "/dev/null";
constexpr int first_free_descriptor = STDERR_FILENO + 1;

std::string describe_errno(int code) { return std::system_category().message(code); }

// A descriptor that is closed when it goes, unless it was handed on.
class Descriptor {
public:
    Descriptor() = default;
    explicit Descriptor(int fd) noexcept : fd_(fd) {}
    Descriptor(Descriptor&& other) noexcept : fd_(other.release()) {}
    Descriptor& operator=(Descriptor&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.release();
        }
        return *this;
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    ~Descriptor() { reset(); }

    int get() const noexcept { return fd_; }
    int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }
    void reset() noexcept {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

private:
    int fd_ = -1;
};

// A pipe whose ends are close-on-exec and above the standard descriptors, so that no duplication onto 0, 1 or 2 in
// the child overwrites another end. Linux makes the ends close-on-exec as it creates them; elsewhere a process that
// another thread starts between the two steps may inherit them, which costs that process two descriptors and this
// pipe nothing, since the child started here closes every descriptor but its standard ones.
std::optional<std::string> make_pipe(Descriptor& read_end, Descriptor& write_end) {
    int ends[2] = {-1, -1};
#ifdef __linux__
    if (::pipe2(ends, O_CLOEXEC) != 0) return "a pipe could not be made: " + describe_errno(errno);
#else
    if (::pipe(ends) != 0) return "a pipe could not be made: " + describe_errno(errno);
    for (const int end : ends) ::fcntl(end, F_SETFD, ::fcntl(end, F_GETFD) | FD_CLOEXEC);
#endif
    Descriptor kept[2] = {Descriptor(ends[0]), Descriptor(ends[1])};
    for (Descriptor& end : kept) {
        if (end.get() >= first_free_descriptor) continue;
        const int moved = ::fcntl(end.get(), F_DUPFD_CLOEXEC, first_free_descriptor);
        if (moved < 0) return "a pipe could not be made: " + describe_errno(errno);
        end = Descriptor(moved);
    }
    read_end = std::move(kept[0]);
    write_end = std::move(kept[1]);
    return std::nullopt;
}

bool executable(const std::filesystem::path& candidate) {
    std::error_code error;
    return std::filesystem::is_regular_file(candidate, error) && ::access(candidate.c_str(), X_OK) == 0;
}

// A path that is relative is taken from the directory the child runs in: its working directory when one is given.
std::filesystem::path from_child_directory(const std::filesystem::path& path,
                                           const std::optional<std::filesystem::path>& working_directory) {
    std::filesystem::path full = path;
    if (full.is_relative() && working_directory) full = *working_directory / full;
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(full, error);
    return error ? full : absolute;
}

// Finds the program the way the transport describes: a name with a directory as a path, a bare name through the PATH
// of the child's environment.
std::variant<std::filesystem::path, std::string> find_program(const ChildLaunch& launch) {
    const std::filesystem::path named(launch.program);
    if (launch.program.find(directory_separator) != std::string::npos) {
        std::filesystem::path candidate = from_child_directory(named, launch.working_directory);
        if (executable(candidate)) return candidate;
        return "the program \"" + launch.program + "\" is not an executable file";
    }
    const std::string* search = nullptr;
    for (const auto& [name, value] : launch.environment) {
        if (name == path_variable) search = &value;
    }
    if (search != nullptr) {
        std::string_view rest = *search;
        while (true) {
            const std::size_t end = rest.find(path_separator);
            const std::string_view directory = rest.substr(0, end);
            // An empty entry names no directory here.
            if (!directory.empty()) {
                std::filesystem::path candidate =
                    from_child_directory(std::filesystem::path(std::string(directory)) / named,
                                         launch.working_directory);
                if (executable(candidate)) return candidate;
            }
            if (end == std::string_view::npos) break;
            rest.remove_prefix(end + 1);
        }
    }
    return "the program \"" + launch.program + "\" was not found on the PATH of its environment";
}

// The attributes and file actions of one start, destroyed when it goes.
class SpawnSetup {
public:
    SpawnSetup() {
        actions_ready_ = ::posix_spawn_file_actions_init(&actions_) == 0;
        attributes_ready_ = ::posix_spawnattr_init(&attributes_) == 0;
    }
    SpawnSetup(const SpawnSetup&) = delete;
    SpawnSetup& operator=(const SpawnSetup&) = delete;
    ~SpawnSetup() {
        if (actions_ready_) ::posix_spawn_file_actions_destroy(&actions_);
        if (attributes_ready_) ::posix_spawnattr_destroy(&attributes_);
    }
    bool ready() const noexcept { return actions_ready_ && attributes_ready_; }
    posix_spawn_file_actions_t* actions() noexcept { return &actions_; }
    posix_spawnattr_t* attributes() noexcept { return &attributes_; }

private:
    posix_spawn_file_actions_t actions_;
    posix_spawnattr_t attributes_;
    bool actions_ready_ = false;
    bool attributes_ready_ = false;
};

}  // namespace

Child::Child(std::int64_t pid, ChildPipe input, ChildPipe output, std::optional<ChildPipe> error)
    : pid_(pid), input_(std::move(input)), output_(std::move(output)), error_(std::move(error)) {}

std::variant<std::unique_ptr<Child>, std::string> Child::start(const asio::any_io_executor& executor,
                                                                const ChildLaunch& launch, OnExit on_exit) {
    auto found = find_program(launch);
    if (auto* problem = std::get_if<std::string>(&found)) return std::move(*problem);
    const std::filesystem::path program = std::get<std::filesystem::path>(std::move(found));

    Descriptor input_read, input_write, output_read, output_write, error_read, error_write;
    if (auto problem = make_pipe(input_read, input_write)) return std::move(*problem);
    if (auto problem = make_pipe(output_read, output_write)) return std::move(*problem);
    if (launch.stderr_mode == StderrMode::capture) {
        if (auto problem = make_pipe(error_read, error_write)) return std::move(*problem);
    }

    SpawnSetup setup;
    if (!setup.ready()) return std::string("the start of a process could not be prepared");
    posix_spawn_file_actions_t* actions = setup.actions();
    posix_spawnattr_t* attributes = setup.attributes();
    int failed = ::posix_spawn_file_actions_adddup2(actions, input_read.get(), STDIN_FILENO);
    if (failed == 0) failed = ::posix_spawn_file_actions_adddup2(actions, output_write.get(), STDOUT_FILENO);
    if (failed == 0) {
        switch (launch.stderr_mode) {
            case StderrMode::capture:
                failed = ::posix_spawn_file_actions_adddup2(actions, error_write.get(), STDERR_FILENO);
                break;
            case StderrMode::discard:
                failed = ::posix_spawn_file_actions_addopen(actions, STDERR_FILENO, null_device, O_WRONLY, 0);
                break;
            case StderrMode::inherit:
#ifdef __APPLE__
                failed = ::posix_spawn_file_actions_addinherit_np(actions, STDERR_FILENO);
#endif
                break;
        }
    }
    if (failed == 0 && launch.working_directory) {
        failed = ::posix_spawn_file_actions_addchdir_np(actions, launch.working_directory->c_str());
    }
#ifndef __APPLE__
    // Every descriptor but the standard three is closed in the child (on Apple systems the flag below does it).
    if (failed == 0) failed = ::posix_spawn_file_actions_addclosefrom_np(actions, first_free_descriptor);
#endif
    short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef __APPLE__
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    // A process group of its own, whose number is the child's process id; no signal blocked, and every signal at its
    // default disposition, so that what the client ignores (SIGPIPE among them) the server does not.
    sigset_t none;
    sigset_t every;
    sigemptyset(&none);
    sigfillset(&every);
    sigdelset(&every, SIGKILL);
    sigdelset(&every, SIGSTOP);
    if (failed == 0) failed = ::posix_spawnattr_setflags(attributes, flags);
    if (failed == 0) failed = ::posix_spawnattr_setpgroup(attributes, 0);
    if (failed == 0) failed = ::posix_spawnattr_setsigmask(attributes, &none);
    if (failed == 0) failed = ::posix_spawnattr_setsigdefault(attributes, &every);
    if (failed != 0) return "the start of a process could not be prepared: " + describe_errno(failed);

    std::vector<std::string> argument_texts;
    argument_texts.reserve(launch.arguments.size() + 1);
    argument_texts.push_back(launch.program);
    for (const std::string& argument : launch.arguments) argument_texts.push_back(argument);
    std::vector<char*> argv;
    for (std::string& text : argument_texts) argv.push_back(text.data());
    argv.push_back(nullptr);
    std::vector<std::string> variable_texts;
    variable_texts.reserve(launch.environment.size());
    for (const auto& [name, value] : launch.environment) variable_texts.push_back(name + "=" + value);
    std::vector<char*> envp;
    for (std::string& text : variable_texts) envp.push_back(text.data());
    envp.push_back(nullptr);

    pid_t pid = 0;
    failed = ::posix_spawn(&pid, program.c_str(), actions, attributes, argv.data(), envp.data());
    // The child's ends belong to the child alone: with them closed here, the client sees the end of the output once
    // every process that holds it has ended.
    input_read.reset();
    output_write.reset();
    error_write.reset();
    if (failed != 0) return "the program \"" + launch.program + "\" could not be started: " + describe_errno(failed);
#ifdef __APPLE__
    // A write to a pipe the child closed fails with EPIPE instead of raising SIGPIPE.
    ::fcntl(input_write.get(), F_SETNOSIGPIPE, 1);
#endif

    // From here on the process runs and must be collected, whatever else fails.
    std::unique_ptr<Child> child;
    try {
        std::optional<ChildPipe> error;
        if (error_read.get() >= 0) error.emplace(executor, error_read.release());
        child.reset(new Child(pid, ChildPipe(executor, input_write.release()), ChildPipe(executor, output_read.release()),
                              std::move(error)));
        child->waiter_ = std::thread([pid, executor, notify = std::move(on_exit)]() mutable {
            siginfo_t info;
            int result = 0;
            do {
                std::memset(&info, 0, sizeof info);
                // WNOWAIT leaves the process a zombie: its process id, and so the number of its group, stays taken
                // until the client collects it, after it has signalled the group for the last time.
                result = ::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOWAIT);
            } while (result != 0 && errno == EINTR);
            const bool observed = result == 0;
            asio::post(executor, [notify = std::move(notify), observed] { notify(observed); });
        });
    } catch (...) {
        ::kill(-pid, SIGKILL);
        if (child) {
            child->collected_ = true;
            if (child->waiter_.joinable()) child->waiter_.join();
        }
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        return std::string("the process could not be watched");
    }
    return child;
}

Child::~Child() {
    if (collected_) return;
    ::kill(-static_cast<pid_t>(pid_), SIGKILL);
    if (waiter_.joinable()) waiter_.join();
    int status = 0;
    while (::waitpid(static_cast<pid_t>(pid_), &status, 0) < 0 && errno == EINTR) {
    }
}

void Child::terminate() noexcept {
    if (!collected_) ::kill(-static_cast<pid_t>(pid_), SIGTERM);
}

void Child::kill() noexcept {
    if (!collected_) ::kill(-static_cast<pid_t>(pid_), SIGKILL);
}

ChildExit Child::collect(bool observed) {
    ChildExit how;
    if (collected_) return how;
    collected_ = true;
    if (waiter_.joinable()) waiter_.join();
    if (!observed) return how;
    int status = 0;
    pid_t result = 0;
    do {
        result = ::waitpid(static_cast<pid_t>(pid_), &status, 0);
    } while (result < 0 && errno == EINTR);
    if (result != static_cast<pid_t>(pid_)) return how;
    if (WIFEXITED(status)) how.exit_code = WEXITSTATUS(status);
    if (WIFSIGNALED(status)) how.signal = WTERMSIG(status);
    return how;
}

}  // namespace neomcp::detail

#endif
