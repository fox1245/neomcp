// A server for the tests of the child-process transport that does what its script says, so that a test can make a
// server misbehave in each way it needs without timing anything: every step is started by an event (the start, a
// line read, the end of the input, a signal).
//
// Usage: neomcp_scripted_stdio_server <script.json> [arguments...]
//
// The script is a JSON object:
//   "start":  actions run when the process starts.
//   "rules":  [{"method": m, "name": n, "actions": [...]}, ...]: for each line read that is a JSON object whose
//             "method" is m (and, when "name" is given, whose params.name is n), the actions of the first rule that
//             matches.
//   "other":  actions for a line that no rule matches (default: none).
//   "end":    actions run when the input ends (default: [{"exit": 0}]). When they do not exit, the process waits for a
//             signal for ever.
// Actions, each an object with one member:
//   {"answer": result}             the line is a request: answer it with this result
//   {"refuse": {"code", "message"}} answer it with this error
//   {"sized_answer": {"result": r, "bytes": n, "end": e}}   answer it with r, padded with spaces after the message to
//                                  n bytes, and end the line with e (such as "\n" or "\r\n")
//   {"line": text}                 write text and a line feed to standard output
//   {"raw": text}                  write text as it is to standard output
//   {"lines": {"text": t, "count": n}}   write t and a line feed n times
//   {"long_line": n}               write a line of n letters
//   {"stderr": text}               write text as it is to standard error
//   {"stderr_lines": {"text": t, "count": n}}
//   {"close_stdout": true}         close standard output and go on
//   {"close_stdin": true}          close standard input and go on (the end of the input follows)
//   {"exit": status}               exit at once with this status
//   {"append": {"path": p, "text": t}}   append t and a line feed to the file p
//   {"report": path}               write a JSON object to the file: "arguments" (every argument after the script),
//                                  "environment" (an object of every variable), "directory" (the working directory),
//                                  and on POSIX "sigpipe_default" and "sigterm_default" (whether the disposition is
//                                  the default) and "blocked" (whether any signal is blocked)
//   {"ignore_sigterm": true}       POSIX: SIGTERM is ignored from now on
//   {"holder": path}               POSIX: start a child that ignores SIGTERM and holds the standard streams and the
//                                  process group, and write its process id to the file once it ignores SIGTERM
//   {"burst_on_sigusr1": {"text": t, "count": n}}   POSIX: from now on SIGUSR1 makes the process write t and a line
//                                  feed n times to standard output and exit with status 0, from the signal handler
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <neojson/json.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
extern char** environ;
#endif

namespace {

using json = neojson::json;

std::vector<std::string> arguments_after_script;

void write_out(const std::string& text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

void write_err(const std::string& text) {
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fflush(stderr);
}

void append_to(const std::string& path, const std::string& text) {
    std::ofstream file(path, std::ios::app | std::ios::binary);
    file << text << '\n';
}

[[noreturn]] void wait_for_ever() {
#ifdef _WIN32
    for (;;) ::SleepEx(INFINITE, TRUE);
#else
    for (;;) ::pause();
#endif
}

json environment_object() {
    json variables = json::object();
#ifdef _WIN32
    wchar_t* block = ::GetEnvironmentStringsW();
    for (const wchar_t* entry = block; entry != nullptr && *entry != L'\0'; entry += std::wcslen(entry) + 1) {
        const std::wstring wide(entry);
        const int length = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                                                 nullptr, nullptr);
        std::string text(static_cast<std::size_t>(length > 0 ? length : 0), '\0');
        if (length > 0) {
            ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), length, nullptr,
                                  nullptr);
        }
        // Entries for drives ("=C:=C:\") begin with '=': the name is what comes before the next '='.
        const std::size_t equals = text.find('=', 1);
        if (equals == std::string::npos) continue;
        variables[text.substr(0, equals)] = text.substr(equals + 1);
    }
    if (block != nullptr) ::FreeEnvironmentStringsW(block);
#else
    for (char** entry = environ; *entry != nullptr; ++entry) {
        const std::string text(*entry);
        const std::size_t equals = text.find('=');
        if (equals == std::string::npos) continue;
        variables[text.substr(0, equals)] = text.substr(equals + 1);
    }
#endif
    return variables;
}

void report(const std::string& path) {
    json out = json::object();
    json arguments = json::array();
    for (const std::string& argument : arguments_after_script) arguments.push_back(argument);
    out["arguments"] = arguments;
    out["environment"] = environment_object();
    const std::u8string directory = std::filesystem::current_path().generic_u8string();
    out["directory"] = std::string(directory.begin(), directory.end());
#ifndef _WIN32
    struct sigaction pipe_action = {};
    struct sigaction term_action = {};
    ::sigaction(SIGPIPE, nullptr, &pipe_action);
    ::sigaction(SIGTERM, nullptr, &term_action);
    out["sigpipe_default"] = pipe_action.sa_handler == SIG_DFL;
    out["sigterm_default"] = term_action.sa_handler == SIG_DFL;
    sigset_t blocked;
    sigemptyset(&blocked);
    ::sigprocmask(SIG_BLOCK, nullptr, &blocked);
    bool any = false;
    for (int signal_number = 1; signal_number < NSIG; ++signal_number) {
        if (sigismember(&blocked, signal_number) == 1) any = true;
    }
    out["blocked"] = any;
#endif
    // Written whole under another name and then renamed, so that a reader never sees half of it.
    const std::string partial = path + ".partial";
    {
        std::ofstream file(partial, std::ios::binary | std::ios::trunc);
        file << out.dump();
    }
    std::error_code ignored;
    std::filesystem::rename(partial, path, ignored);
}

#ifndef _WIN32
// What SIGUSR1 writes before the process exits; made before the handler is installed.
std::string burst;

extern "C" void write_burst(int) {
    std::size_t written = 0;
    while (written < burst.size()) {
        const ssize_t count = ::write(STDOUT_FILENO, burst.data() + written, burst.size() - written);
        if (count <= 0) break;
        written += static_cast<std::size_t>(count);
    }
    ::_exit(0);
}

void start_holder(const std::string& path) {
    int ready[2];
    if (::pipe(ready) != 0) std::exit(90);
    const pid_t holder = ::fork();
    if (holder < 0) std::exit(91);
    if (holder == 0) {
        ::close(ready[0]);
        ::signal(SIGTERM, SIG_IGN);
        const char byte = 'h';
        if (::write(ready[1], &byte, 1) != 1) ::_exit(92);
        ::close(ready[1]);
        for (;;) ::pause();
    }
    ::close(ready[1]);
    char byte = 0;
    if (::read(ready[0], &byte, 1) != 1) std::exit(93);
    ::close(ready[0]);
    const std::string partial = path + ".partial";
    {
        std::ofstream file(partial, std::ios::binary | std::ios::trunc);
        file << holder;
    }
    std::error_code ignored;
    std::filesystem::rename(partial, path, ignored);
}
#endif

void run(const json& actions, const json* message) {
    if (!actions.is_array()) return;
    for (const json& action : actions) {
        if (!action.is_object() || action.size() != 1) continue;
        const auto entry = action.begin();
        const std::string& kind = entry.key();
        const json& value = entry.value();
        if (kind == "answer" || kind == "refuse") {
            if (message == nullptr || !message->contains("id")) continue;
            json reply = json::object();
            reply["jsonrpc"] = "2.0";
            reply["id"] = (*message)["id"];
            reply[kind == "answer" ? "result" : "error"] = value;
            write_out(reply.dump() + "\n");
        } else if (kind == "sized_answer") {
            if (message == nullptr || !message->contains("id")) continue;
            json reply = json::object();
            reply["jsonrpc"] = "2.0";
            reply["id"] = (*message)["id"];
            reply["result"] = value["result"];
            std::string text = reply.dump();
            const auto bytes = value["bytes"].get<std::size_t>();
            if (text.size() < bytes) text.append(bytes - text.size(), ' ');
            write_out(text + value["end"].get<std::string>());
        } else if (kind == "line") {
            write_out(value.get<std::string>() + "\n");
        } else if (kind == "raw") {
            write_out(value.get<std::string>());
        } else if (kind == "lines") {
            const std::string text = value["text"].get<std::string>() + "\n";
            const auto count = value["count"].get<std::size_t>();
            std::string all;
            for (std::size_t at = 0; at < count; ++at) all += text;
            write_out(all);
        } else if (kind == "long_line") {
            write_out(std::string(value.get<std::size_t>(), 'x') + "\n");
        } else if (kind == "stderr") {
            write_err(value.get<std::string>());
        } else if (kind == "stderr_lines") {
            const std::string text = value["text"].get<std::string>() + "\n";
            const auto count = value["count"].get<std::size_t>();
            std::string all;
            for (std::size_t at = 0; at < count; ++at) all += text;
            write_err(all);
        } else if (kind == "close_stdout") {
            std::fflush(stdout);
#ifdef _WIN32
            ::_close(1);
#else
            ::close(1);
#endif
        } else if (kind == "close_stdin") {
#ifdef _WIN32
            ::_close(0);
#else
            ::close(0);
#endif
        } else if (kind == "exit") {
            std::fflush(stdout);
            std::fflush(stderr);
            std::_Exit(value.get<int>());
        } else if (kind == "append") {
            append_to(value["path"].get<std::string>(), value["text"].get<std::string>());
        } else if (kind == "report") {
            report(value.get<std::string>());
        } else if (kind == "ignore_sigterm") {
#ifndef _WIN32
            ::signal(SIGTERM, SIG_IGN);
#endif
        } else if (kind == "burst_on_sigusr1") {
#ifndef _WIN32
            const std::string text = value["text"].get<std::string>() + "\n";
            const auto count = value["count"].get<std::size_t>();
            burst.clear();
            for (std::size_t at = 0; at < count; ++at) burst += text;
            struct sigaction on_signal = {};
            on_signal.sa_handler = write_burst;
            sigemptyset(&on_signal.sa_mask);
            ::sigaction(SIGUSR1, &on_signal, nullptr);
#endif
        } else if (kind == "holder") {
#ifndef _WIN32
            start_holder(value.get<std::string>());
#endif
        }
    }
}

bool matches(const json& rule, const json& message) {
    if (!rule.contains("method")) return false;
    if (!message.contains("method") || message["method"] != rule["method"]) return false;
    if (rule.contains("name")) {
        if (!message.contains("params") || !message["params"].is_object()) return false;
        const json& params = message["params"];
        if (!params.contains("name") || params["name"] != rule["name"]) return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    ::_setmode(0, _O_BINARY);
    ::_setmode(1, _O_BINARY);
    ::_setmode(2, _O_BINARY);
#endif
    if (argc < 2) {
        write_err("usage: neomcp_scripted_stdio_server <script.json> [arguments...]\n");
        return 2;
    }
    for (int at = 2; at < argc; ++at) arguments_after_script.emplace_back(argv[at]);
    std::ifstream file(argv[1], std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const json script = json::parse(text, nullptr, false);
    if (script.is_discarded() || !script.is_object()) {
        write_err("the script is not a JSON object\n");
        return 2;
    }
    if (script.contains("start")) run(script["start"], nullptr);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const json message = json::parse(line, nullptr, false);
        bool handled = false;
        if (!message.is_discarded() && message.is_object() && script.contains("rules")) {
            for (const json& rule : script["rules"]) {
                if (!matches(rule, message)) continue;
                run(rule["actions"], &message);
                handled = true;
                break;
            }
        }
        if (!handled && script.contains("other")) run(script["other"], message.is_discarded() ? nullptr : &message);
    }
    if (script.contains("end")) {
        run(script["end"], nullptr);
        wait_for_ever();
    }
    return 0;
}
