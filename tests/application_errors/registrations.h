// What the servers of the application-error tests serve: a handler of every kind (tool, resource, resource template,
// prompt, completer), plain and as a coroutine, each of which throws neomcp::ApplicationError with the code it is given
// and a message and data that name the kind of handler. The stdio program (fixture.cpp) and the tests that serve in
// process register the same.
#pragma once

#include <charconv>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <asio/awaitable.hpp>

#include "neomcp/server.h"

namespace application_errors {

using neomcp::json;

struct CodeArguments {
    int code;
};

struct CodeText {
    std::string code;
};

struct CodeTexts {
    std::string code;
    std::string code_coroutine;
};

}  // namespace application_errors

NEOMCP_DESCRIBE(application_errors::CodeArguments, code, "the error code to answer with")
NEOMCP_DESCRIBE(application_errors::CodeText, code, "the error code to answer with")
NEOMCP_DESCRIBE(application_errors::CodeTexts, code, "the error code to answer with",
                code_coroutine, "the error code to answer with, from a coroutine")

namespace application_errors {

// The code of the one handler that has no variable to carry it.
inline constexpr int fixed_code = -32050;

// The names of the handlers: what the message and the data of the error say, so that a test sees which handler's own
// answer came back.
inline constexpr std::string_view kind_tool = "tool";
inline constexpr std::string_view kind_tool_coroutine = "tool_coroutine";
inline constexpr std::string_view kind_resource = "resource";
inline constexpr std::string_view kind_resource_coroutine = "resource_coroutine";
inline constexpr std::string_view kind_template = "template";
inline constexpr std::string_view kind_template_coroutine = "template_coroutine";
inline constexpr std::string_view kind_prompt = "prompt";
inline constexpr std::string_view kind_prompt_coroutine = "prompt_coroutine";
inline constexpr std::string_view kind_completion = "completion";
inline constexpr std::string_view kind_completion_coroutine = "completion_coroutine";
inline constexpr std::string_view kind_completion_template = "completion_template";

inline std::string message_of(std::string_view kind) { return "the " + std::string(kind) + " handler's own words"; }

inline json data_of(std::string_view kind, int code) {
    json list = json::array();
    list.push_back(1);
    list.push_back("two");
    json inner = json::object();
    inner["three"] = 3;
    list.push_back(std::move(inner));
    json data = json::object();
    data["kind"] = std::string(kind);
    data["code"] = code;
    data["detail"] = std::move(list);
    return data;
}

// Throws the error of the handler `kind`. It returns a type, so that a handler ends in `return fail_as<T>(...)` and
// has no statement after the call that does not return.
template <class T>
[[noreturn]] T fail_as(std::string_view kind, int code) {
    throw neomcp::ApplicationError(code, message_of(kind), data_of(kind, code));
}

inline int code_of(std::string_view text) {
    int code = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), code);
    if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size()) {
        throw std::invalid_argument("not a code");
    }
    return code;
}

// The template of the variable `code`; reads of its URIs and completions of its variable both fail.
inline constexpr std::string_view template_uri = "fail://template/{code}";
inline constexpr std::string_view template_coroutine_uri = "fail://template-coroutine/{code}";
inline constexpr std::string_view fixed_uri = "fail://fixed";
inline constexpr std::string_view fixed_coroutine_uri = "fail://fixed-coroutine";

inline void register_failing(neomcp::Server& server) {
    server.tool<CodeArguments>("fail", "Answers with an error of its own",
                               [](const CodeArguments& arguments) -> std::string {
                                   return fail_as<std::string>(kind_tool, arguments.code);
                               });
    server.tool<CodeArguments>("fail_coroutine", "Answers with an error of its own, from a coroutine",
                               [](const CodeArguments& arguments) -> asio::awaitable<std::string> {
                                   co_return fail_as<std::string>(kind_tool_coroutine, arguments.code);
                               });

    server.resource(std::string(fixed_uri), "A resource whose read fails",
                    []() -> std::string { return fail_as<std::string>(kind_resource, fixed_code); });
    server.resource(std::string(fixed_coroutine_uri), "A resource whose read fails, from a coroutine",
                    []() -> asio::awaitable<std::string> {
                        co_return fail_as<std::string>(kind_resource_coroutine, fixed_code);
                    });
    server
        .resource_template<CodeArguments>(std::string(template_uri), "A template whose reads fail",
                                          [](const CodeArguments& arguments) -> std::string {
                                              return fail_as<std::string>(kind_template, arguments.code);
                                          })
        .complete("code", [](std::string_view typed) -> std::vector<std::string> {
            return fail_as<std::vector<std::string>>(kind_completion_template, code_of(typed));
        });
    server.resource_template<CodeArguments>(std::string(template_coroutine_uri),
                                            "A template whose reads fail, from a coroutine",
                                            [](const CodeArguments& arguments) -> asio::awaitable<std::string> {
                                                co_return fail_as<std::string>(kind_template_coroutine, arguments.code);
                                            });

    server.prompt<CodeText>("fail_prompt", "A prompt that fails", [](const CodeText& arguments) -> std::string {
        return fail_as<std::string>(kind_prompt, code_of(arguments.code));
    });
    server.prompt<CodeText>("fail_prompt_coroutine", "A prompt that fails, from a coroutine",
                            [](const CodeText& arguments) -> asio::awaitable<std::string> {
                                co_return fail_as<std::string>(kind_prompt_coroutine, code_of(arguments.code));
                            });
    server
        .prompt<CodeTexts>("fail_completion", "A prompt whose completions fail",
                           [](const CodeTexts&) -> std::string { return std::string("unused"); })
        .complete("code", [](std::string_view typed) -> std::vector<std::string> {
            return fail_as<std::vector<std::string>>(kind_completion, code_of(typed));
        })
        .complete("code_coroutine", [](std::string typed) -> asio::awaitable<std::vector<std::string>> {
            co_return fail_as<std::vector<std::string>>(kind_completion_coroutine, code_of(typed));
        });
}

// Tools that fail with the code they are given, and may run as tasks, plain and as a coroutine.
inline void register_task_tools(neomcp::Server& server) {
    server
        .tool<CodeArguments>("task_fail", "Fails with an error of its own",
                             [](const CodeArguments& arguments) -> std::string {
                                 return fail_as<std::string>(kind_tool, arguments.code);
                             })
        .task_support(neomcp::TaskSupport::optional);
    server
        .tool<CodeArguments>("task_fail_coroutine", "Fails with an error of its own, from a coroutine",
                             [](const CodeArguments& arguments) -> asio::awaitable<std::string> {
                                 co_return fail_as<std::string>(kind_tool_coroutine, arguments.code);
                             })
        .task_support(neomcp::TaskSupport::optional);
}

}  // namespace application_errors
