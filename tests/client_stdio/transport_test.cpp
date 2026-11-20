// The child-process transport on its own, driven by a rig that plays the client: starting, the environment, lines,
// the bounds, standard error, the steps that end a process, and starting a new one.
#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "client_stdio/child.h"
#include "support.h"

#ifdef _WIN32
#include <stdlib.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace client_stdio_test;
using neomcp::ChildProcessCause;
using neomcp::ChildProcessStep;

namespace {

// One transport and the rig that drives it; the transport is closed when the test ends, also when an assertion ended
// it early.
struct Session {
    explicit Session(neomcp::ChildProcess process,
                     std::size_t max_message_bytes = neomcp::ClientDefaults::max_message_bytes) {
        rig.max_message_bytes = max_message_bytes;
        transport = transport_of(std::move(process), steps, max_message_bytes);
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session() {
        if (transport && started && !closed) close();
        transport.reset();
    }

    std::optional<std::string> start() {
        started = true;
        return rig.start(*transport);
    }
    void close(neomcp::CloseCause cause = neomcp::CloseCause::client_closed) {
        closed = true;
        if (rig.close(*transport, cause)) return;
        // A close that never completes would leave the transport running under the test: the program ends here, and
        // with it (POSIX) the groups of the processes the transport started.
        std::fprintf(stderr, "the transport did not close within the bound\n");
#ifndef _WIN32
        for (const auto& event : steps.all()) {
            if (event.step == ChildProcessStep::started) ::kill(-static_cast<pid_t>(event.pid), SIGKILL);
        }
#endif
        std::fflush(stderr);
        std::_Exit(1);
    }

    Rig rig;
    Steps steps;
    std::unique_ptr<neomcp::Transport> transport;
    bool started = false;
    bool closed = false;
};

std::string request(int id, std::string_view method) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", std::string(method)}}.dump();
}

void set_variable(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    ::setenv(name.c_str(), value.c_str(), 1);
#endif
}

void unset_variable(const std::string& name) {
#ifdef _WIN32
    _putenv_s(name.c_str(), "");
#else
    ::unsetenv(name.c_str());
#endif
}

bool has_variable(const json& environment, std::string_view name) {
    for (auto entry = environment.begin(); entry != environment.end(); ++entry) {
        if (neomcp::detail::same_variable(entry.key(), name)) return true;
    }
    return false;
}

std::string variable(const json& environment, std::string_view name) {
    for (auto entry = environment.begin(); entry != environment.end(); ++entry) {
        if (neomcp::detail::same_variable(entry.key(), name)) return entry.value().get<std::string>();
    }
    return {};
}

const json ready = action("line", R"({"ready":true})");

}  // namespace

// ---- Describing and starting

TEST(ClientStdioTransport, DescriptionsThatCannotWorkAreRefused) {
    const auto refused = [](neomcp::ChildProcess process, neomcp::ChildProcessTransportOptions options = {}) {
        auto made = neomcp::make_transport(std::move(process), std::move(options));
        return !made && made.error().kind == neomcp::ClientErrorKind::invalid_options;
    };
    neomcp::ChildProcess empty;
    EXPECT_TRUE(refused(empty));
    neomcp::ChildProcess nul_argument;
    nul_argument.program = "server";
    nul_argument.arguments.push_back(std::string("a\0b", 3));
    EXPECT_TRUE(refused(nul_argument));
    neomcp::ChildProcess bad_name;
    bad_name.program = "server";
    bad_name.environment.emplace_back("A=B", "c");
    EXPECT_TRUE(refused(bad_name));
    neomcp::ChildProcess empty_name;
    empty_name.program = "server";
    empty_name.inherited = std::vector<std::string>{""};
    EXPECT_TRUE(refused(empty_name));
    neomcp::ChildProcess no_line;
    no_line.program = "server";
    no_line.stderr_mode = neomcp::StderrMode::capture;
    no_line.max_stderr_line_bytes = 0;
    EXPECT_TRUE(refused(no_line));
    neomcp::ChildProcess negative;
    negative.program = "server";
    negative.term_grace = std::chrono::milliseconds(-1);
    EXPECT_TRUE(refused(negative));
    neomcp::ChildProcess fine;
    fine.program = "server";
    neomcp::ChildProcessTransportOptions zero;
    zero.max_message_bytes = 0;
    EXPECT_TRUE(refused(fine, zero));
    EXPECT_FALSE(refused(fine));
}

TEST(ClientStdioTransport, AProgramThatCannotBeFoundFailsTheStart) {
    Folder folder;
    neomcp::ChildProcess process;
    process.program = "neomcp-no-such-program-anywhere";
    Session session(process);
    ASSERT_TRUE(session.transport);
    const auto problem = session.start();
    ASSERT_TRUE(problem.has_value());
    EXPECT_NE(problem->find("was not found"), std::string::npos) << *problem;
    EXPECT_EQ(session.steps.count(ChildProcessStep::started), 0u);

    neomcp::ChildProcess missing_path;
    missing_path.program = (folder.path() / "missing" / "server").string();
    Session second(missing_path);
    ASSERT_TRUE(second.start().has_value());
}

TEST(ClientStdioTransport, ArgumentsReachTheProgramUnchanged) {
    Folder folder;
    const std::string report = folder.file("report.json");
    const std::vector<std::string> arguments{"two words", "a \"quoted\" word", "trailing\\", "", "$HOME ; | * `x`",
                                             "back\\\\slashes\\\"", "\t"};
    Session session(scripted(folder, json{{"start", json::array({action("report", report)})}}, arguments));
    ASSERT_FALSE(session.start().has_value());
    const auto seen = await_json(report);
    ASSERT_TRUE(seen.has_value());
    std::vector<std::string> received;
    for (const json& argument : (*seen)["arguments"]) received.push_back(argument.get<std::string>());
    EXPECT_EQ(received, arguments);
}

TEST(ClientStdioTransport, TheChildGetsTheNamedVariablesAndTheAddedOnesOnly) {
    Folder folder;
    const std::string report = folder.file("report.json");
    set_variable("NEOMCP_CLIENT_STDIO_SECRET", "kept-here");
    neomcp::ChildProcess process = scripted(folder, json{{"start", json::array({action("report", report)})}});
    process.environment.emplace_back("NEOMCP_CLIENT_STDIO_ADDED", "added");
    process.environment.emplace_back("PATH", "replaced-path");
    Session session(process);
    ASSERT_FALSE(session.start().has_value());
    const auto seen = await_json(report);
    unset_variable("NEOMCP_CLIENT_STDIO_SECRET");
    ASSERT_TRUE(seen.has_value());
    const json& environment = (*seen)["environment"];
    EXPECT_FALSE(has_variable(environment, "NEOMCP_CLIENT_STDIO_SECRET"));
    EXPECT_EQ(variable(environment, "NEOMCP_CLIENT_STDIO_ADDED"), "added");
    // A name given in both takes the added value.
    EXPECT_EQ(variable(environment, "PATH"), "replaced-path");
    // Every other variable is one of the default list.
    for (auto entry = environment.begin(); entry != environment.end(); ++entry) {
        if (neomcp::detail::same_variable(entry.key(), "NEOMCP_CLIENT_STDIO_ADDED")) continue;
        bool listed = false;
        for (const std::string_view name : neomcp::inherited_environment()) {
            listed = listed || neomcp::detail::same_variable(entry.key(), name);
        }
        EXPECT_TRUE(listed) << entry.key();
    }
}

TEST(ClientStdioTransport, InheritedNamesReplaceTheDefaultList) {
    Folder folder;
    const std::string report = folder.file("report.json");
    set_variable("NEOMCP_CLIENT_STDIO_PASSED", "passed");
    neomcp::ChildProcess process = scripted(folder, json{{"start", json::array({action("report", report)})}});
    std::vector<std::string> names{"NEOMCP_CLIENT_STDIO_PASSED"};
#ifdef _WIN32
    // A Windows program needs this one to start at all.
    names.emplace_back("SYSTEMROOT");
#endif
    process.inherited = names;
    Session session(process);
    ASSERT_FALSE(session.start().has_value());
    const auto seen = await_json(report);
    unset_variable("NEOMCP_CLIENT_STDIO_PASSED");
    ASSERT_TRUE(seen.has_value());
    const json& environment = (*seen)["environment"];
    EXPECT_EQ(variable(environment, "NEOMCP_CLIENT_STDIO_PASSED"), "passed");
    EXPECT_EQ(environment.size(), names.size()) << environment.dump();
}

TEST(ClientStdioTransport, TheProcessRunsInTheWorkingDirectoryGiven) {
    Folder folder;
    const std::string report = folder.file("report.json");
    neomcp::ChildProcess process = scripted(folder, json{{"start", json::array({action("report", report)})}});
    std::filesystem::create_directories(folder.path() / "inside");
    process.working_directory = folder.path() / "inside";
    Session session(process);
    ASSERT_FALSE(session.start().has_value());
    const auto seen = await_json(report);
    ASSERT_TRUE(seen.has_value());
    EXPECT_TRUE(std::filesystem::equivalent(std::filesystem::path((*seen)["directory"].get<std::string>()),
                                            folder.path() / "inside"));
}

#ifndef _WIN32
TEST(ClientStdioTransport, TheChildStartsWithEverySignalAtItsDefault) {
    Folder folder;
    const std::string report = folder.file("report.json");
    // What the client's process ignores or blocks, the server does not.
    struct sigaction ignore = {};
    ignore.sa_handler = SIG_IGN;
    struct sigaction before = {};
    ::sigaction(SIGTERM, &ignore, &before);
    Session session(scripted(folder, json{{"start", json::array({action("report", report)})}}));
    const auto problem = session.start();
    ::sigaction(SIGTERM, &before, nullptr);
    ASSERT_FALSE(problem.has_value());
    const auto seen = await_json(report);
    ASSERT_TRUE(seen.has_value());
    EXPECT_TRUE((*seen)["sigpipe_default"].get<bool>());
    EXPECT_TRUE((*seen)["sigterm_default"].get<bool>());
    EXPECT_FALSE((*seen)["blocked"].get<bool>());
}

TEST(ClientStdioTransport, TheChildHoldsNoOtherDescriptorOfTheClient) {
    Folder folder;
    // A descriptor of the client without close-on-exec.
    const int opened = ::open(folder.file("held").c_str(), O_CREAT | O_WRONLY, 0600);
    ASSERT_GE(opened, 0);
    // Far above what the listing itself opens.
    const int open_here = ::fcntl(opened, F_DUPFD, 200);
    ::close(opened);
    ASSERT_GE(open_here, 200);
    const std::string report = folder.file("descriptors");
    neomcp::ChildProcess process;
    process.program = "/bin/sh";
    process.arguments = {"-c", "ls /proc/self/fd > \"$0\" 2>/dev/null || ls /dev/fd > \"$0\"", report};
    Session session(process);
    ASSERT_FALSE(session.start().has_value());
    ASSERT_TRUE(eventually([&] { return session.rig.exits().size() == 1; }));
    ::close(open_here);
    std::set<std::string> seen;
    std::string name;
    for (const char character : read_file(report)) {
        if (character == '\n') {
            if (!name.empty()) seen.insert(name);
            name.clear();
        } else {
            name.push_back(character);
        }
    }
    // The shell's own descriptor for the listing may appear as well; the client's may not.
    EXPECT_EQ(seen.count(std::to_string(open_here)), 0u);
}
#endif

// ---- Lines

TEST(ClientStdioTransport, LinesArriveWithoutTheirTerminatorsAndTheLastOneWithoutALineFeedToo) {
    Folder folder;
    Session session(scripted(folder, json{{"start", json::array({action("raw", "{\"a\":1}\r\n\n{\"b\":2}\n{\"c\":3}"),
                                                                 action("exit", 0)})}}));
    ASSERT_FALSE(session.start().has_value());
    ASSERT_TRUE(eventually([&] { return session.rig.exits().size() == 1; }));
    EXPECT_EQ(session.rig.lines(), (std::vector<std::string>{R"({"a":1})", R"({"b":2})", R"({"c":3})"}));
    // Everything the process wrote before it ended arrives before its end.
    EXPECT_EQ(session.rig.order(), (std::vector<std::string>{"line", "line", "line", "exited"}));
    ASSERT_TRUE(session.rig.exits().front().status.has_value());
    EXPECT_EQ(*session.rig.exits().front().status, 0);
}

TEST(ClientStdioTransport, AMessageGoesOutAsOneLine) {
    Folder folder;
    Session session(scripted(folder, json{{"rules", json::array({rule("echo", json::array({action("answer", "echoed")}))})}}));
    ASSERT_FALSE(session.start().has_value());
    session.rig.send(*session.transport, request(41, "echo"));
    ASSERT_TRUE(eventually([&] { return !session.rig.lines().empty(); }));
    const json answer = json::parse(session.rig.lines().front());
    EXPECT_EQ(answer["id"], 41);
    EXPECT_EQ(answer["result"], "echoed");
}

TEST(ClientStdioTransport, LinesThatAreNotMessagesAreSkippedWithinTheBounds) {
    Folder folder;
    neomcp::ChildProcess process =
        scripted(folder, json{{"start", json::array({action("lines", json{{"text", "a banner"}, {"count", 3}}),
                                                     action("line", R"({"after":"banners"})")})}});
    process.max_ignored_stdout_lines = 3;
    Session session(process);
    ASSERT_FALSE(session.start().has_value());
    ASSERT_TRUE(eventually([&] { return session.rig.lines().size() == 4; }));
    EXPECT_EQ(session.rig.lines().back(), R"({"after":"banners"})");
    EXPECT_EQ(session.steps.count(ChildProcessStep::kill_sent), 0u);
}

TEST(ClientStdioTransport, OneLineMoreThanTheBoundEndsTheProcess) {
    Folder folder;
    neomcp::ChildProcess process =
        scripted(folder, json{{"start", json::array({action("lines", json{{"text", "a banner"}, {"count", 4}}),
                                                     action("line", R"({"never":"read"})")})},
                              {"end", json::array()}});
    process.max_ignored_stdout_lines = 3;
    process.exit_grace = long_grace;
    Session session(process);
    ASSERT_FALSE(session.start().has_value());
    const auto killed = session.steps.await(ChildProcessStep::kill_sent);
    ASSERT_TRUE(killed.has_value());
    EXPECT_EQ(killed->cause, ChildProcessCause::output_refused);
    ASSERT_TRUE(eventually([&] { return session.rig.exits().size() == 1; }));
    EXPECT_EQ(session.rig.lines().size(), 4u);
}

TEST(ClientStdioTransport, TheBytesOfSkippedLinesAreBoundedToo) {
    Folder folder;
    neomcp::ChildProcess process =
        scripted(folder, json{{"start", json::array({action("line", std::string(40, 'b'))})}, {"end", json::array()}});
    process.max_ignored_stdout_bytes = 39;
    Session session(process);
    ASSERT_FALSE(session.start().has_value());
    const auto killed = session.steps.await(ChildProcessStep::kill_sent);
    ASSERT_TRUE(killed.has_value());
    EXPECT_EQ(killed->cause, ChildProcessCause::output_refused);

    neomcp::ChildProcess within =
        scripted(folder, json{{"start", json::array({action("line", std::string(40, 'b')), ready})}});
    within.max_ignored_stdout_bytes = 40;
    Session fine(within);
    ASSERT_FALSE(fine.start().has_value());
    ASSERT_TRUE(eventually([&] { return fine.rig.lines().size() == 2; }));
    EXPECT_EQ(fine.steps.count(ChildProcessStep::kill_sent), 0u);
}

TEST(ClientStdioTransport, ALineLongerThanAMessageIsSkippedUnreadAndCounted) {
    Folder folder;
    constexpr std::size_t bound = 64;
    neomcp::ChildProcess process =
        scripted(folder, json{{"start", json::array({action("long_line", 100'000), ready})}});
    process.max_ignored_stdout_lines = 1;
    Session session(process, bound);
    ASSERT_FALSE(session.start().has_value());
    ASSERT_TRUE(eventually([&] { return session.rig.lines().size() == 2; }));
    // It is handed over cut one byte past the bound, so that it is seen to be too long, and never held whole.
    EXPECT_EQ(session.rig.lines().front().size(), bound + 1);
    EXPECT_EQ(session.rig.lines().back(), R"({"ready":true})");

    neomcp::ChildProcess none = scripted(folder, json{{"start", json::array({action("long_line", 1000)})},
                                                      {"end", json::array()}});
    none.max_ignored_stdout_lines = 0;
    Session refused(none, bound);
    ASSERT_FALSE(refused.start().has_value());
    const auto killed = refused.steps.await(ChildProcessStep::kill_sent);
    ASSERT_TRUE(killed.has_value());
    EXPECT_EQ(killed->cause, ChildProcessCause::output_refused);
}

// ---- Standard error

TEST(ClientStdioTransport, ACapturedStandardErrorKeepsItsTailAndCutsLongLines) {
    Folder folder;
    std::mutex mutex;
    std::vector<std::string> lines;
    neomcp::ChildProcess process = scripted(
        folder, json{{"start", json::array({action("stderr", "short\nthis line is too long\r\nlast"), action("exit", 0)})}});
    process.stderr_mode = neomcp::StderrMode::capture;
    process.stderr_tail_bytes = 12;
    process.max_stderr_line_bytes = 8;
    process.on_stderr_line = [&](std::string_view line) {
        std::lock_guard lock(mutex);
        lines.emplace_back(line);
    };
    Session session(process);
    ASSERT_FALSE(session.start().has_value());
    const auto exited = session.steps.await(ChildProcessStep::exited);
    ASSERT_TRUE(exited.has_value());
    EXPECT_EQ(exited->stderr_tail, "o long\r\nlast");
    std::lock_guard lock(mutex);
    EXPECT_EQ(lines, (std::vector<std::string>{"short", "this lin", "last"}));
}

TEST(ClientStdioTransport, ACapturedStandardErrorIsReadEvenWithoutACallback) {
    Folder folder;
    // Far more than a pipe holds: a process whose standard error is not read would stop in its write.
    neomcp::ChildProcess process = scripted(
        folder, json{{"start", json::array({action("stderr_lines", json{{"text", std::string(100, 'e')}, {"count", 20'000}}),
                                            ready})}});
    process.stderr_mode = neomcp::StderrMode::capture;
    Session session(process);
    ASSERT_FALSE(session.start().has_value());
    ASSERT_TRUE(eventually([&] { return session.rig.lines().size() == 1; }));
}

#ifndef _WIN32
// The client's standard error for the length of a test: a file, so that what the server writes there can be read.
class ErrorToFile {
public:
    explicit ErrorToFile(const std::string& path) {
        saved_ = ::dup(STDERR_FILENO);
        const int file = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
        ::dup2(file, STDERR_FILENO);
        ::close(file);
    }
    ErrorToFile(const ErrorToFile&) = delete;
    ErrorToFile& operator=(const ErrorToFile&) = delete;
    ~ErrorToFile() { restore(); }
    void restore() {
        if (saved_ < 0) return;
        ::dup2(saved_, STDERR_FILENO);
        ::close(saved_);
        saved_ = -1;
    }

private:
    int saved_ = -1;
};

TEST(ClientStdioTransport, StandardErrorIsInheritedOrDiscardedAsAsked) {
    Folder folder;
    for (const auto mode : {neomcp::StderrMode::inherit, neomcp::StderrMode::discard}) {
        const std::string path = folder.file(mode == neomcp::StderrMode::inherit ? "inherit" : "discard");
        neomcp::ChildProcess process =
            scripted(folder, json{{"start", json::array({action("stderr", "written-to-stderr\n"), action("exit", 0)})}});
        process.stderr_mode = mode;
        {
            ErrorToFile redirect(path);
            Session session(process);
            ASSERT_FALSE(session.start().has_value());
            ASSERT_TRUE(session.steps.await(ChildProcessStep::exited).has_value());
        }
        const bool written = read_file(path).find("written-to-stderr") != std::string::npos;
        EXPECT_EQ(written, mode == neomcp::StderrMode::inherit);
    }
}
#endif

// ---- Ending a process

TEST(ClientStdioTransport, AServerThatExitsWhenItsInputClosesIsNotAskedToTerminate) {
    Folder folder;
    neomcp::ChildProcess process = scripted(folder, json{{"start", json::array({ready})}});
    process.exit_grace = long_grace;
    process.term_grace = long_grace;
    Session session(process);
    ASSERT_FALSE(session.start().has_value());
    ASSERT_TRUE(eventually([&] { return session.rig.lines().size() == 1; }));
    const auto began = std::chrono::steady_clock::now();
    session.close();
    EXPECT_LT(std::chrono::steady_clock::now() - began, long_grace);
    EXPECT_EQ(session.steps.steps(), (std::vector<ChildProcessStep>{ChildProcessStep::started,
                                                                     ChildProcessStep::input_closed,
                                                                     ChildProcessStep::kill_sent,
                                                                     ChildProcessStep::exited}));
    const auto events = session.steps.all();
    EXPECT_EQ(events[1].cause, ChildProcessCause::closing);
    EXPECT_EQ(events[2].cause, ChildProcessCause::process_ended);
    ASSERT_TRUE(events[3].exit_code.has_value());
    EXPECT_EQ(*events[3].exit_code, 0);
    // The client is not told of an end it asked for.
    EXPECT_TRUE(session.rig.exits().empty());
}

TEST(ClientStdioTransport, AServerThatIgnoresItsInputClosingIsEnded) {
    Folder folder;
    neomcp::ChildProcess process = scripted(folder, json{{"start", json::array({ready})}, {"end", json::array()}});
    process.exit_grace = short_grace;
    process.term_grace = long_grace;
    Session session(process);
    ASSERT_FALSE(session.start().has_value());
    ASSERT_TRUE(eventually([&] { return session.rig.lines().size() == 1; }));
    session.close();
    const auto events = session.steps.all();
#ifdef _WIN32
    ASSERT_EQ(session.steps.steps(), (std::vector<ChildProcessStep>{ChildProcessStep::started,
                                                                     ChildProcessStep::input_closed,
                                                                     ChildProcessStep::kill_sent,
                                                                     ChildProcessStep::exited}));
    EXPECT_EQ(events[2].cause, ChildProcessCause::exit_grace_lapsed);
#else
    // SIGTERM ends it, at its default disposition; what is left of its group is ended after it.
    ASSERT_EQ(session.steps.steps(), (std::vector<ChildProcessStep>{ChildProcessStep::started,
                                                                     ChildProcessStep::input_closed,
                                                                     ChildProcessStep::terminate_sent,
                                                                     ChildProcessStep::kill_sent,
                                                                     ChildProcessStep::exited}));
    EXPECT_EQ(events[2].cause, ChildProcessCause::exit_grace_lapsed);
    EXPECT_EQ(events[3].cause, ChildProcessCause::process_ended);
    ASSERT_TRUE(events[4].signal.has_value());
    EXPECT_EQ(*events[4].signal, SIGTERM);
#endif
}

#ifndef _WIN32
TEST(ClientStdioTransport, AServerThatIgnoresSigtermIsKilled) {
    Folder folder;
    neomcp::ChildProcess process =
        scripted(folder, json{{"start", json::array({action("ignore_sigterm", true), ready})}, {"end", json::array()}});
    process.exit_grace = short_grace;
    process.term_grace = short_grace;
    Session session(process);
    ASSERT_FALSE(session.start().has_value());
    ASSERT_TRUE(eventually([&] { return session.rig.lines().size() == 1; }));
    session.close();
    ASSERT_EQ(session.steps.steps(), (std::vector<ChildProcessStep>{ChildProcessStep::started,
                                                                     ChildProcessStep::input_closed,
                                                                     ChildProcessStep::terminate_sent,
                                                                     ChildProcessStep::kill_sent,
                                                                     ChildProcessStep::exited}));
    const auto events = session.steps.all();
    EXPECT_EQ(events[3].cause, ChildProcessCause::term_grace_lapsed);
    ASSERT_TRUE(events[4].signal.has_value());
    EXPECT_EQ(*events[4].signal, SIGKILL);
}

// Whether a process is gone: no such process, or (Linux) one that has ended and waits to be collected by its parent.
bool process_gone(pid_t pid) {
    if (::kill(pid, 0) != 0) return errno == ESRCH;
    const std::string stat = read_file("/proc/" + std::to_string(pid) + "/stat");
    const std::size_t name_end = stat.rfind(')');
    if (name_end == std::string::npos || name_end + 2 >= stat.size()) return false;
    const char state = stat[name_end + 2];
    return state == 'Z' || state == 'X';
}

// Ends a process the test started indirectly when the test ends, also when an assertion ended it early.
class EndsWithTheTest {
public:
    explicit EndsWithTheTest(pid_t pid) : pid_(pid) {}
    EndsWithTheTest(const EndsWithTheTest&) = delete;
    EndsWithTheTest& operator=(const EndsWithTheTest&) = delete;
    ~EndsWithTheTest() {
        if (pid_ > 0 && !process_gone(pid_)) ::kill(pid_, SIGKILL);
    }

private:
    pid_t pid_;
};

TEST(ClientStdioTransport, TheGroupIsEndedAfterItsLeaderIsGone) {
    Folder folder;
    const std::string holder_file = folder.file("holder");
    neomcp::ChildProcess process =
        scripted(folder, json{{"start", json::array({action("holder", holder_file), ready})}, {"end", json::array()}});
    process.exit_grace = short_grace;
    process.term_grace = long_grace;
    Session session(process);
    ASSERT_FALSE(session.start().has_value());
    ASSERT_TRUE(eventually([&] { return session.rig.lines().size() == 1; }));
    ASSERT_TRUE(eventually([&] { return std::filesystem::exists(holder_file); }));
    const pid_t holder = static_cast<pid_t>(std::stol(read_file(holder_file)));
    ASSERT_GT(holder, 0);
    const EndsWithTheTest end_holder(holder);
    // The leader ends on SIGTERM; the process it started ignores it, and holds the group and the output.
    session.close();
    ASSERT_EQ(session.steps.steps(), (std::vector<ChildProcessStep>{ChildProcessStep::started,
                                                                     ChildProcessStep::input_closed,
                                                                     ChildProcessStep::terminate_sent,
                                                                     ChildProcessStep::kill_sent,
                                                                     ChildProcessStep::exited}));
    EXPECT_EQ(session.steps.all()[3].cause, ChildProcessCause::process_ended);
    EXPECT_TRUE(eventually([&] { return process_gone(holder); }));
}

TEST(ClientStdioTransport, AProcessThatEndsByItselfTakesItsGroupWithIt) {
    Folder folder;
    const std::string holder_file = folder.file("holder");
    neomcp::ChildProcess process =
        scripted(folder, json{{"start", json::array({action("holder", holder_file), ready})},
                              {"rules", json::array({rule("die", json::array({action("exit", 4)}))})}});
    process.exit_grace = long_grace;
    Session session(process);
    ASSERT_FALSE(session.start().has_value());
    ASSERT_TRUE(eventually([&] { return std::filesystem::exists(holder_file); }));
    const pid_t holder = static_cast<pid_t>(std::stol(read_file(holder_file)));
    ASSERT_GT(holder, 0);
    const EndsWithTheTest end_holder(holder);
    session.rig.send(*session.transport, request(1, "die"));
    // The holder kept the output open: the end is reported once the group was ended, not after the grace.
    ASSERT_TRUE(eventually([&] { return session.rig.exits().size() == 1; }));
    ASSERT_TRUE(session.rig.exits().front().status.has_value());
    EXPECT_EQ(*session.rig.exits().front().status, 4);
    EXPECT_TRUE(eventually([&] { return process_gone(holder); }));
}

// Whether a child of this process has ended, without collecting it.
bool child_ended(pid_t pid) {
    siginfo_t info = {};
    return ::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOWAIT | WNOHANG) == 0 && info.si_pid == pid;
}

TEST(ClientStdioTransport, OutputStillInThePipeWhenTheProcessEndsIsReadBeforeTheEnd) {
    Folder folder;
    constexpr std::size_t burst_lines = 200;
    neomcp::ChildProcess process = scripted(
        folder, json{{"start", json::array({action("burst_on_sigusr1", json{{"text", R"({"burst":true})"},
                                                                            {"count", burst_lines}}),
                                            ready})},
                     {"end", json::array()}});
    process.exit_grace = long_grace;
    Session session(process);
    ASSERT_FALSE(session.start().has_value());
    ASSERT_TRUE(eventually([&] { return session.rig.lines().size() == 1; }));
    const auto started = session.steps.await(ChildProcessStep::started);
    ASSERT_TRUE(started.has_value());
    const auto pid = static_cast<pid_t>(started->pid);
    {
        // The client's thread is held while the process writes its last lines and exits: its end is noticed (and
        // queued) before a read of those lines can run.
        auto release = session.rig.hold();
        ::kill(pid, SIGUSR1);
        EXPECT_TRUE(eventually([&] { return child_ended(pid); }));
        release->set_value();
    }
    ASSERT_TRUE(eventually([&] { return session.rig.exits().size() == 1; }));
    EXPECT_EQ(session.rig.lines().size(), burst_lines + 1);
    EXPECT_EQ(session.rig.order().back(), "exited");
}

TEST(ClientStdioTransport, AProcessWhoseInputBrokeIsEnded) {
    Folder folder;
    neomcp::ChildProcess process =
        scripted(folder, json{{"start", json::array({action("close_stdin", true), ready})}, {"end", json::array()}});
    process.exit_grace = long_grace;
    Session session(process);
    ASSERT_FALSE(session.start().has_value());
    ASSERT_TRUE(eventually([&] { return session.rig.lines().size() == 1; }));
    session.rig.send(*session.transport, request(1, "anything"));
    const auto killed = session.steps.await(ChildProcessStep::kill_sent);
    ASSERT_TRUE(killed.has_value());
    EXPECT_EQ(killed->cause, ChildProcessCause::input_failed);
    EXPECT_TRUE(eventually([&] { return session.rig.exits().size() == 1; }));
}
#endif

TEST(ClientStdioTransport, ContactEndedEndsTheProcessAtOnce) {
    Folder folder;
    neomcp::ChildProcess process = scripted(folder, json{{"start", json::array({ready})}, {"end", json::array()}});
    process.exit_grace = long_grace;
    process.term_grace = long_grace;
    Session session(process);
    ASSERT_FALSE(session.start().has_value());
    ASSERT_TRUE(eventually([&] { return session.rig.lines().size() == 1; }));
    session.close(neomcp::CloseCause::contact_ended);
    ASSERT_EQ(session.steps.steps(), (std::vector<ChildProcessStep>{ChildProcessStep::started,
                                                                     ChildProcessStep::input_closed,
                                                                     ChildProcessStep::kill_sent,
                                                                     ChildProcessStep::exited}));
    EXPECT_EQ(session.steps.all()[1].cause, ChildProcessCause::contact_ended);
    EXPECT_EQ(session.steps.all()[2].cause, ChildProcessCause::contact_ended);
}

TEST(ClientStdioTransport, AProcessThatClosedItsOutputAndRunsOnIsEnded) {
    Folder folder;
    neomcp::ChildProcess process =
        scripted(folder, json{{"start", json::array({ready, action("close_stdout", true)})}, {"end", json::array()}});
    process.exit_grace = short_grace;
    Session session(process);
    ASSERT_FALSE(session.start().has_value());
    const auto killed = session.steps.await(ChildProcessStep::kill_sent);
    ASSERT_TRUE(killed.has_value());
    EXPECT_EQ(killed->cause, ChildProcessCause::output_closed);
    EXPECT_TRUE(eventually([&] { return session.rig.exits().size() == 1; }));
}

// ---- An end the client did not ask for, and a new process

TEST(ClientStdioTransport, AnEndIsReportedWithItsStatusAfterWhatTheProcessWrote) {
    Folder folder;
    neomcp::ChildProcess process = scripted(
        folder, json{{"rules", json::array({rule("die", json::array({action("answer", "last words"), action("exit", 7)}))})}});
    Session session(process);
    ASSERT_FALSE(session.start().has_value());
    session.rig.send(*session.transport, request(3, "die"));
    ASSERT_TRUE(eventually([&] { return session.rig.exits().size() == 1; }));
    EXPECT_EQ(session.rig.order(), (std::vector<std::string>{"line", "exited"}));
    ASSERT_TRUE(session.rig.exits().front().status.has_value());
    EXPECT_EQ(*session.rig.exits().front().status, 7);
    const auto exited = session.steps.await(ChildProcessStep::exited);
    ASSERT_TRUE(exited.has_value());
    ASSERT_TRUE(exited->exit_code.has_value());
    EXPECT_EQ(*exited->exit_code, 7);

    // Nothing runs now: a message has nowhere to go.
    session.rig.send(*session.transport, request(4, "die"));
    EXPECT_TRUE(eventually([&] { return session.rig.send_failures() == 1; }));
}

TEST(ClientStdioTransport, ARestartStartsANewProcess) {
    Folder folder;
    neomcp::ChildProcess process = scripted(
        folder, json{{"start", json::array({ready})}, {"rules", json::array({rule("die", json::array({action("exit", 1)}))})}});
    Session session(process);
    ASSERT_FALSE(session.start().has_value());
    session.rig.send(*session.transport, request(1, "die"));
    ASSERT_TRUE(eventually([&] { return session.rig.exits().size() == 1; }));
    session.rig.send(*session.transport, std::string(), neomcp::OutgoingKind::restart);
    ASSERT_TRUE(eventually([&] { return session.rig.restarts() == 1; }));
    const auto first = session.steps.await(ChildProcessStep::started, 1);
    const auto second = session.steps.await(ChildProcessStep::started, 2);
    ASSERT_TRUE(first && second);
    EXPECT_EQ(second->cause, ChildProcessCause::restart);
    EXPECT_NE(first->pid, second->pid);
    // The new process is spoken to.
    ASSERT_TRUE(eventually([&] { return session.rig.lines().size() == 2; }));
}

TEST(ClientStdioTransport, ARestartThatCannotStartIsAnEndAtOnce) {
    Folder folder;
    const std::filesystem::path copy = folder.path() / ("server-copy" + std::filesystem::path(scripted_server).extension().string());
    std::filesystem::copy_file(scripted_server, copy);
    neomcp::ChildProcess process =
        scripted(folder, json{{"rules", json::array({rule("die", json::array({action("exit", 1)}))})}});
    process.program = copy.string();
    Session session(process);
    ASSERT_FALSE(session.start().has_value());
    session.rig.send(*session.transport, request(1, "die"));
    ASSERT_TRUE(eventually([&] { return session.rig.exits().size() == 1; }));
    std::error_code removed;
    std::filesystem::remove(copy, removed);
    if (removed) GTEST_SKIP() << "the copy of the program could not be removed: " << removed.message();
    session.rig.send(*session.transport, std::string(), neomcp::OutgoingKind::restart);
    ASSERT_TRUE(eventually([&] { return session.rig.exits().size() == 2; }));
    EXPECT_EQ(session.rig.restarts(), 0u);
    EXPECT_FALSE(session.rig.exits().back().status.has_value());
}

// ---- The Windows command line, built and checked on every platform

TEST(ClientStdioCommandLine, EveryArgumentIsQuotedAndItsBackslashesAndQuotesEscaped) {
    using neomcp::detail::windows_command_line;
    EXPECT_EQ(windows_command_line({L"C:\\a b\\server.exe"}), L"\"C:\\a b\\server.exe\"");
    EXPECT_EQ(windows_command_line({L"p", L""}), L"\"p\" \"\"");
    EXPECT_EQ(windows_command_line({L"p", L"two words"}), L"\"p\" \"two words\"");
    EXPECT_EQ(windows_command_line({L"p", L"say \"hi\""}), L"\"p\" \"say \\\"hi\\\"\"");
    // Backslashes count only before a quote: doubled there and before the closing quote, kept elsewhere.
    EXPECT_EQ(windows_command_line({L"p", L"a\\b"}), L"\"p\" \"a\\b\"");
    EXPECT_EQ(windows_command_line({L"p", L"trailing\\"}), L"\"p\" \"trailing\\\\\"");
    EXPECT_EQ(windows_command_line({L"p", L"x\\\"y"}), L"\"p\" \"x\\\\\\\"y\"");
}
