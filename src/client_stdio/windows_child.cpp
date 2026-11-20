// A child process on Windows: CreateProcessW, started suspended and put in a job object before it runs, with
// overlapped named pipes for its standard streams and its process handle waited on through asio.
#ifdef _WIN32

#include "client_stdio/child.h"

#include <asio/error.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <string>
#include <system_error>
#include <vector>

namespace neomcp::detail {

namespace {

constexpr std::wstring_view path_variable = L"PATH";
constexpr wchar_t path_separator = L';';
// What CreateProcessW itself appends to a program named without an extension.
constexpr std::wstring_view default_extension = L".exe";
// Files that CreateProcessW runs through the command interpreter, which would be a shell after all.
constexpr std::wstring_view refused_extensions[] = {L".bat", L".cmd"};
constexpr const wchar_t* null_device = L"NUL";
constexpr const wchar_t* pipe_prefix = L"\\\\.\\pipe\\neomcp-client-";
constexpr DWORD pipe_buffer_bytes = 64 * 1024;
// The exit status of the processes of a job that is terminated.
constexpr UINT terminated_status = 1;

std::string describe_error(DWORD code) { return std::system_category().message(static_cast<int>(code)); }

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE handle) noexcept : handle_(handle) {}
    Handle(Handle&& other) noexcept : handle_(other.release()) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.release();
        }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }

    HANDLE get() const noexcept { return handle_; }
    bool valid() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
    HANDLE release() noexcept {
        HANDLE handle = handle_;
        handle_ = nullptr;
        return handle;
    }
    void reset() noexcept {
        if (valid()) ::CloseHandle(handle_);
        handle_ = nullptr;
    }

private:
    HANDLE handle_ = nullptr;
};

bool same_text_ignoring_case(std::wstring_view first, std::wstring_view second) {
    return ::CompareStringOrdinal(first.data(), static_cast<int>(first.size()), second.data(),
                                  static_cast<int>(second.size()), TRUE) == CSTR_EQUAL;
}

bool refused(const std::filesystem::path& candidate) {
    const std::wstring extension = candidate.extension().wstring();
    for (const std::wstring_view refused_extension : refused_extensions) {
        if (same_text_ignoring_case(extension, refused_extension)) return true;
    }
    return false;
}

bool runnable(const std::filesystem::path& candidate) {
    std::error_code error;
    return std::filesystem::is_regular_file(candidate, error);
}

std::filesystem::path from_child_directory(const std::filesystem::path& path,
                                           const std::optional<std::filesystem::path>& working_directory) {
    std::filesystem::path full = path;
    if (full.is_relative() && working_directory) full = *working_directory / full;
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(full, error);
    return error ? full : absolute;
}

// The candidate itself when it has an extension, else with the one CreateProcessW appends.
std::filesystem::path with_extension(std::filesystem::path candidate) {
    if (!candidate.has_extension()) candidate += std::wstring(default_extension);
    return candidate;
}

std::variant<std::filesystem::path, std::string> find_program(const ChildLaunch& launch,
                                                              const std::filesystem::path& named,
                                                              const std::vector<std::pair<std::wstring, std::wstring>>& environment) {
    const auto check = [&launch](const std::filesystem::path& candidate)
        -> std::optional<std::variant<std::filesystem::path, std::string>> {
        if (!runnable(candidate)) return std::nullopt;
        if (refused(candidate)) {
            return std::variant<std::filesystem::path, std::string>(
                "the program \"" + launch.program + "\" is a batch file, which would run through the command interpreter");
        }
        return std::variant<std::filesystem::path, std::string>(candidate);
    };
    if (named.has_parent_path() || named.has_root_path()) {
        if (auto found = check(with_extension(from_child_directory(named, launch.working_directory)))) return *found;
        return "the program \"" + launch.program + "\" is not a file";
    }
    const std::wstring* search = nullptr;
    for (const auto& [name, value] : environment) {
        if (same_text_ignoring_case(name, path_variable)) search = &value;
    }
    if (search != nullptr) {
        std::wstring_view rest = *search;
        while (true) {
            const std::size_t end = rest.find(path_separator);
            const std::wstring_view directory = rest.substr(0, end);
            if (!directory.empty()) {
                const std::filesystem::path candidate =
                    from_child_directory(std::filesystem::path(std::wstring(directory)) / named, launch.working_directory);
                if (auto found = check(with_extension(candidate))) return *found;
            }
            if (end == std::wstring_view::npos) break;
            rest.remove_prefix(end + 1);
        }
    }
    return "the program \"" + launch.program + "\" was not found on the PATH of its environment";
}

// The environment block: NAME=VALUE strings sorted by name without regard to case, each ended by a NUL, and a NUL
// after the last.
std::wstring environment_block(std::vector<std::pair<std::wstring, std::wstring>> environment) {
    std::sort(environment.begin(), environment.end(), [](const auto& first, const auto& second) {
        return ::CompareStringOrdinal(first.first.c_str(), -1, second.first.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    std::wstring block;
    for (const auto& [name, value] : environment) {
        block += name;
        block += L'=';
        block += value;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

// One pipe: the client's end overlapped (for asio), the child's end inheritable and synchronous, as a child's standard
// handles are. `client_reads` decides the direction.
std::optional<std::string> make_pipe(bool client_reads, Handle& client_end, Handle& child_end) {
    static std::atomic<unsigned long long> counter{0};
    const std::wstring name = std::wstring(pipe_prefix) + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                              std::to_wstring(counter.fetch_add(1) + 1);
    const DWORD direction = client_reads ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND;
    // The first instance only: a pipe of the same name made by anyone else makes this fail instead of being used.
    Handle server(::CreateNamedPipeW(name.c_str(), direction | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                     PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                     pipe_buffer_bytes, pipe_buffer_bytes, 0, nullptr));
    if (!server.valid()) return "a pipe could not be made: " + describe_error(::GetLastError());
    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = static_cast<DWORD>(sizeof(inherit));
    inherit.bInheritHandle = TRUE;
    Handle client(::CreateFileW(name.c_str(), client_reads ? GENERIC_WRITE : GENERIC_READ, 0, &inherit, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!client.valid()) return "a pipe could not be made: " + describe_error(::GetLastError());
    client_end = std::move(server);
    child_end = std::move(client);
    return std::nullopt;
}

std::optional<std::string> inheritable_null(Handle& handle) {
    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = static_cast<DWORD>(sizeof(inherit));
    inherit.bInheritHandle = TRUE;
    handle = Handle(::CreateFileW(null_device, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle.valid()) return "the null device could not be opened: " + describe_error(::GetLastError());
    return std::nullopt;
}

// The client's own standard error, as an inheritable duplicate; the null device when the client has none.
std::optional<std::string> inherited_error(Handle& handle) {
    const HANDLE own = ::GetStdHandle(STD_ERROR_HANDLE);
    if (own == nullptr || own == INVALID_HANDLE_VALUE) return inheritable_null(handle);
    HANDLE duplicate = nullptr;
    if (!::DuplicateHandle(::GetCurrentProcess(), own, ::GetCurrentProcess(), &duplicate, 0, TRUE,
                           DUPLICATE_SAME_ACCESS)) {
        return inheritable_null(handle);
    }
    handle = Handle(duplicate);
    return std::nullopt;
}

class AttributeList {
public:
    AttributeList() = default;
    AttributeList(const AttributeList&) = delete;
    AttributeList& operator=(const AttributeList&) = delete;
    ~AttributeList() {
        if (ready_) ::DeleteProcThreadAttributeList(list());
    }

    // Lets the child inherit these handles and no other.
    bool restrict_to(HANDLE* handles, std::size_t count) {
        SIZE_T size = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        storage_.resize(size);
        if (!::InitializeProcThreadAttributeList(list(), 1, 0, &size)) return false;
        ready_ = true;
        return ::UpdateProcThreadAttribute(list(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                           count * sizeof(HANDLE), nullptr, nullptr) != FALSE;
    }

    LPPROC_THREAD_ATTRIBUTE_LIST list() noexcept {
        return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
    }

private:
    std::vector<unsigned char> storage_;
    bool ready_ = false;
};

}  // namespace

Child::Child(std::int64_t pid, ChildPipe input, ChildPipe output, std::optional<ChildPipe> error,
             asio::windows::object_handle process, void* job)
    : process_(std::move(process)),
      job_(job),
      pid_(pid),
      input_(std::move(input)),
      output_(std::move(output)),
      error_(std::move(error)) {}

std::variant<std::unique_ptr<Child>, std::string> Child::start(const asio::any_io_executor& executor,
                                                                const ChildLaunch& launch, OnExit on_exit) {
    const auto program_text = widen(launch.program);
    if (!program_text) return std::string("the program is not valid UTF-8");
    std::vector<std::pair<std::wstring, std::wstring>> environment;
    for (const auto& [name, value] : launch.environment) {
        auto wide_name = widen(name);
        auto wide_value = widen(value);
        if (!wide_name || !wide_value) return std::string("a variable of the environment is not valid UTF-8");
        environment.emplace_back(std::move(*wide_name), std::move(*wide_value));
    }
    auto found = find_program(launch, std::filesystem::path(*program_text), environment);
    if (auto* problem = std::get_if<std::string>(&found)) return std::move(*problem);
    const std::filesystem::path program = std::get<std::filesystem::path>(std::move(found));

    std::vector<std::wstring> argv;
    argv.push_back(*program_text);
    for (const std::string& argument : launch.arguments) {
        auto wide = widen(argument);
        if (!wide) return std::string("an argument is not valid UTF-8");
        argv.push_back(std::move(*wide));
    }
    std::wstring command_line = windows_command_line(argv);
    std::wstring block = environment_block(std::move(environment));

    Handle input_client, input_child, output_client, output_child, error_client, error_child;
    if (auto problem = make_pipe(false, input_client, input_child)) return std::move(*problem);
    if (auto problem = make_pipe(true, output_client, output_child)) return std::move(*problem);
    switch (launch.stderr_mode) {
        case StderrMode::capture:
            if (auto problem = make_pipe(true, error_client, error_child)) return std::move(*problem);
            break;
        case StderrMode::discard:
            if (auto problem = inheritable_null(error_child)) return std::move(*problem);
            break;
        case StderrMode::inherit:
            if (auto problem = inherited_error(error_child)) return std::move(*problem);
            break;
    }

    HANDLE inherited[3] = {input_child.get(), output_child.get(), error_child.get()};
    AttributeList attributes;
    if (!attributes.restrict_to(inherited, 3)) {
        return "the start of a process could not be prepared: " + describe_error(::GetLastError());
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = static_cast<DWORD>(sizeof(startup));
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input_child.get();
    startup.StartupInfo.hStdOutput = output_child.get();
    startup.StartupInfo.hStdError = error_child.get();
    startup.lpAttributeList = attributes.list();

    std::wstring directory;
    if (launch.working_directory) directory = launch.working_directory->wstring();

    Handle job(::CreateJobObjectW(nullptr, nullptr));
    if (!job.valid()) return "a job object could not be made: " + describe_error(::GetLastError());
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    // When the last handle of the job is closed (the client ends), every process of the job ends.
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, static_cast<DWORD>(sizeof(limits)))) {
        return "a job object could not be set up: " + describe_error(::GetLastError());
    }

    PROCESS_INFORMATION started{};
    const DWORD flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW;
    const std::wstring application = program.wstring();
    if (!::CreateProcessW(application.c_str(), command_line.data(), nullptr, nullptr, TRUE, flags, block.data(),
                          directory.empty() ? nullptr : directory.c_str(), &startup.StartupInfo, &started)) {
        return "the program \"" + launch.program + "\" could not be started: " + describe_error(::GetLastError());
    }
    Handle process(started.hProcess);
    Handle thread(started.hThread);
    // The process is in the job before its first instruction runs, so that whatever it starts is in the job as well.
    if (!::AssignProcessToJobObject(job.get(), process.get())) {
        const DWORD code = ::GetLastError();
        ::TerminateProcess(process.get(), terminated_status);
        ::WaitForSingleObject(process.get(), INFINITE);
        return "the process could not be put in a job object: " + describe_error(code);
    }
    if (::ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        const DWORD code = ::GetLastError();
        ::TerminateJobObject(job.get(), terminated_status);
        ::WaitForSingleObject(process.get(), INFINITE);
        return "the process could not be resumed: " + describe_error(code);
    }
    thread.reset();
    input_child.reset();
    output_child.reset();
    error_child.reset();

    std::unique_ptr<Child> child;
    try {
        std::optional<ChildPipe> error;
        if (error_client.valid()) error.emplace(executor, error_client.release());
        ChildPipe input(executor, input_client.release());
        ChildPipe output(executor, output_client.release());
        asio::windows::object_handle waited(executor, process.release());
        child.reset(new Child(static_cast<std::int64_t>(started.dwProcessId), std::move(input), std::move(output),
                              std::move(error), std::move(waited), job.release()));
        child->process_.async_wait([notify = std::move(on_exit)](const std::error_code& waited_for) {
            // Cancelled only when the child is destroyed, which collects it itself.
            if (waited_for == asio::error::operation_aborted) return;
            notify(true);
        });
    } catch (...) {
        if (child) return std::string("the process could not be watched");
        if (job.valid()) ::TerminateJobObject(job.get(), terminated_status);
        return std::string("the process could not be watched");
    }
    return child;
}

Child::~Child() {
    HANDLE job = static_cast<HANDLE>(job_);
    if (!collected_ && job != nullptr) {
        ::TerminateJobObject(job, terminated_status);
        if (process_.is_open()) ::WaitForSingleObject(process_.native_handle(), INFINITE);
    }
    // Closing the last handle of the job ends what is left in it.
    if (job != nullptr) ::CloseHandle(job);
}

void Child::terminate() noexcept {}

void Child::kill() noexcept {
    if (!collected_ && job_ != nullptr) ::TerminateJobObject(static_cast<HANDLE>(job_), terminated_status);
}

ChildExit Child::collect(bool) {
    ChildExit how;
    if (collected_) return how;
    collected_ = true;
    DWORD code = 0;
    if (process_.is_open() && ::GetExitCodeProcess(process_.native_handle(), &code)) {
        how.exit_code = static_cast<int>(code);
    }
    return how;
}

}  // namespace neomcp::detail

#endif
