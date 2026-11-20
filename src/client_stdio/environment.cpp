// The environment of a child process, and the Windows command line. Both are pure functions of their inputs and of
// the client's environment, so that they are tested on every platform where they are built.
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "client_stdio/child.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cstdlib>
#endif

namespace neomcp {

namespace {

// The variables a child takes from the client's environment unless the application names others. The lists are the
// ones the official SDKs' stdio clients pass:
// - POSIX: the Python SDK's stdio client (src/mcp/client/stdio.py, DEFAULT_INHERITED_ENV_VARS, at commit
//   77001c96f9507be0f4d009b83210a384a5e08bda), which is also the TypeScript SDK's list off Windows.
// - Windows: the TypeScript SDK's stdio client (packages/client/src/client/stdio.ts, DEFAULT_INHERITED_ENV_VARS, at
//   commit 5ee54ee30ddc00a914a3d981880f87fdce0e6246). Both SDKs also drop a value that begins with "()" (an exported
//   shell function); this library passes values as they are and leaves the choice of names to the application.
#ifdef _WIN32
constexpr std::array<std::string_view, 18> default_inherited{
    "APPDATA",     "COMSPEC",      "HOMEDRIVE",   "HOMEPATH",    "LOCALAPPDATA",      "PATH",
    "PATHEXT",     "PROCESSOR_ARCHITECTURE",      "PROGRAMDATA", "PROGRAMFILES",      "PROGRAMFILES(X86)",
    "PROGRAMW6432", "SYSTEMDRIVE", "SYSTEMROOT",  "TEMP",        "USERNAME",          "USERPROFILE",
    "WINDIR"};
#else
constexpr std::array<std::string_view, 6> default_inherited{"HOME", "LOGNAME", "PATH", "SHELL", "TERM", "USER"};
#endif

#ifdef _WIN32

// The client's value of a variable; empty when it has none (or one that is not text).
std::optional<std::string> client_variable(const std::string& name) {
    const auto wide_name = detail::widen(name);
    if (!wide_name) return std::nullopt;
    const DWORD needed = ::GetEnvironmentVariableW(wide_name->c_str(), nullptr, 0);
    if (needed == 0) return std::nullopt;
    std::wstring value(needed, L'\0');
    const DWORD written = ::GetEnvironmentVariableW(wide_name->c_str(), value.data(), needed);
    // A variable whose value is empty has `needed` 1 and `written` 0.
    if (written >= needed) return std::nullopt;
    value.resize(written);
    return detail::narrow(value);
}

#else

std::optional<std::string> client_variable(const std::string& name) {
    const char* value = std::getenv(name.c_str());
    if (value == nullptr) return std::nullopt;
    return std::string(value);
}

#endif

}  // namespace

std::span<const std::string_view> inherited_environment() noexcept { return default_inherited; }

namespace detail {

#ifdef _WIN32

std::optional<std::wstring> widen(std::string_view text) {
    if (text.empty()) return std::wstring();
    const int length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                             nullptr, 0);
    if (length <= 0) return std::nullopt;
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(),
                          length);
    return wide;
}

std::optional<std::string> narrow(std::wstring_view text) {
    if (text.empty()) return std::string();
    const int length = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                             nullptr, 0, nullptr, nullptr);
    if (length <= 0) return std::nullopt;
    std::string bytes(static_cast<std::size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), bytes.data(),
                          length, nullptr, nullptr);
    return bytes;
}

#endif

bool same_variable(std::string_view first, std::string_view second) noexcept {
#ifdef _WIN32
    // Windows compares the names of variables without regard to case; the names here are compared as ASCII, which is
    // what every name in the default list is, and a name with other characters compares exactly.
    if (first.size() != second.size()) return false;
    for (std::size_t at = 0; at < first.size(); ++at) {
        char a = first[at];
        char b = second[at];
        if (a >= 'a' && a <= 'z') a = static_cast<char>(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = static_cast<char>(b - 'a' + 'A');
        if (a != b) return false;
    }
    return true;
#else
    return first == second;
#endif
}

Variables child_environment(std::span<const std::string> inherited, const Variables& added) {
    Variables result;
    const auto place = [&result](std::string_view name) -> std::pair<std::string, std::string>* {
        for (auto& variable : result) {
            if (same_variable(variable.first, name)) return &variable;
        }
        return nullptr;
    };
    for (const std::string& name : inherited) {
        if (place(name) != nullptr) continue;
        if (auto value = client_variable(name)) result.emplace_back(name, std::move(*value));
    }
    for (const auto& [name, value] : added) {
        if (auto* existing = place(name)) {
            existing->second = value;
        } else {
            result.emplace_back(name, value);
        }
    }
    return result;
}

std::wstring windows_command_line(const std::vector<std::wstring>& arguments) {
    std::wstring line;
    bool first = true;
    for (const std::wstring& argument : arguments) {
        if (!first) line.push_back(L' ');
        line.push_back(L'"');
        if (first) {
            // The program: read up to the next quote, with no escapes. A path holds no quote.
            line.append(argument);
        } else {
            std::size_t backslashes = 0;
            for (const wchar_t character : argument) {
                if (character == L'\\') {
                    ++backslashes;
                    continue;
                }
                if (character == L'"') {
                    line.append(backslashes * 2 + 1, L'\\');
                } else {
                    line.append(backslashes, L'\\');
                }
                line.push_back(character);
                backslashes = 0;
            }
            // Before the closing quote every backslash is doubled, so that none escapes it.
            line.append(backslashes * 2, L'\\');
        }
        line.push_back(L'"');
        first = false;
    }
    return line;
}

}  // namespace detail

}  // namespace neomcp
