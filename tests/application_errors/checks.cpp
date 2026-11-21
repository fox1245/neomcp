#include "checks.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <optional>
#include <set>
#include <utility>

#include "neomcp/jsonrpc.h"
#include "registrations.h"

namespace application_errors {

namespace {

using neomcp::ClientError;
using neomcp::ClientErrorKind;

// The fixed texts of an internal error that stands for a handler's failure, for a tool and for any other kind.
constexpr std::string_view tool_failed = "Internal error: the tool call failed";
constexpr std::string_view call_failed = "Internal error: the call failed";

struct Case {
    std::string_view kind;
    bool carries_code;  // false: the handler has a code of its own, `fixed_code`
    // Makes the call with the code (or without one); its error, empty when the call did not fail.
    std::function<std::optional<ClientError>(neomcp::Client&, int)> call;
};

template <class R>
std::optional<ClientError> error_of(neomcp::Result<R> result) {
    if (result) return std::nullopt;
    return std::move(result.error());
}

std::vector<Case> cases() {
    std::vector<Case> found;
    const auto tool = [](const char* name) {
        return [name](neomcp::Client& client, int code) {
            return error_of(client.call_tool(name, neomcp::json{{"code", code}}));
        };
    };
    const auto read = [](std::string text) {
        return [text](neomcp::Client& client, int) { return error_of(client.read_resource(text)); };
    };
    const auto read_template = [](std::string_view pattern) {
        const std::string prefix(pattern.substr(0, pattern.find('{')));
        return [prefix](neomcp::Client& client, int code) {
            return error_of(client.read_resource(prefix + std::to_string(code)));
        };
    };
    const auto prompt = [](const char* name) {
        return [name](neomcp::Client& client, int code) {
            return error_of(client.get_prompt(name, {{"code", std::to_string(code)}}));
        };
    };
    const auto complete = [](neomcp::CompletionRef ref, const char* argument) {
        return [ref, argument](neomcp::Client& client, int code) {
            return error_of(client.complete(ref, argument, std::to_string(code)));
        };
    };
    found.push_back({kind_tool, true, tool("fail")});
    found.push_back({kind_tool_coroutine, true, tool("fail_coroutine")});
    found.push_back({kind_resource, false, read(std::string(fixed_uri))});
    found.push_back({kind_resource_coroutine, false, read(std::string(fixed_coroutine_uri))});
    found.push_back({kind_template, true, read_template(template_uri)});
    found.push_back({kind_template_coroutine, true, read_template(template_coroutine_uri)});
    found.push_back({kind_prompt, true, prompt("fail_prompt")});
    found.push_back({kind_prompt_coroutine, true, prompt("fail_prompt_coroutine")});
    found.push_back({kind_completion, true,
                     complete(neomcp::CompletionRef::prompt("fail_completion"), "code")});
    found.push_back({kind_completion_coroutine, true,
                     complete(neomcp::CompletionRef::prompt("fail_completion"), "code_coroutine")});
    found.push_back({kind_completion_template, true,
                     complete(neomcp::CompletionRef::resource(std::string(template_uri)), "code")});
    return found;
}

// The codes the revision names, from its profile: error codes and the ones a sender must not emit.
std::set<int> named_codes(const neomcp::profile::Profile& profile) {
    std::set<int> codes;
    for (const auto& named : profile.error_codes()) codes.insert(named.code);
    for (const int code : profile.not_emitted()) codes.insert(code);
    return codes;
}

}  // namespace

std::vector<std::string> revisions_defining(neomcp::profile::Transport transport) {
    std::vector<std::string> found;
    for (const std::string_view revision : neomcp::profile::builtin_revisions()) {
        const auto profile = neomcp::profile::builtin(revision);
        const bool defined = transport == neomcp::profile::Transport::stdio
                                 ? profile->stdio().has_value()
                                 : transport == neomcp::profile::Transport::http_sse
                                       ? profile->http_sse().has_value()
                                       : profile->streamable_http().has_value();
        if (defined) found.emplace_back(revision);
    }
    return found;
}

neomcp::ClientOptions client_options(const std::string& revision) {
    neomcp::ClientOptions options;
    options.client_info.name = "application-errors";
    options.client_info.version = "1.0.0";
    options.profiles.push_back(neomcp::profile::builtin(revision));
    return options;
}

void run_checks(neomcp::Client& client, const std::string& revision) {
    const auto profile = neomcp::profile::builtin(revision);
    ASSERT_NE(profile, nullptr);
    ASSERT_EQ(client.server().revision, revision);

    const std::set<int> named = named_codes(*profile);
    ASSERT_FALSE(named.empty()) << revision << " names no code";

    // What an application may use under any revision, apart from the codes this one names.
    std::vector<int> allowed = {1, 1000000, -33000, -32099, -32050, -32001, -32000, -31999};
    allowed.erase(std::remove_if(allowed.begin(), allowed.end(), [&](int code) { return named.contains(code); }),
                  allowed.end());
    // What it may not: the protocol's own codes, the edges of the reserved range, and what this revision names.
    std::vector<int> refused = {neomcp::error_code::parse_error,      neomcp::error_code::invalid_request,
                                neomcp::error_code::method_not_found, neomcp::error_code::invalid_params,
                                neomcp::error_code::internal_error,   neomcp::error_code::reserved_range_first,
                                neomcp::error_code::implementation_range_first - 1};
    refused.insert(refused.end(), named.begin(), named.end());

    for (const Case& one : cases()) {
        const std::string label = revision + " " + std::string(one.kind);
        for (const int code : one.carries_code ? allowed : std::vector<int>{fixed_code}) {
            const std::optional<ClientError> error = one.call(client, code);
            const std::string where = label + " code " + std::to_string(code);
            ASSERT_TRUE(error.has_value()) << where << ": the call did not fail";
            EXPECT_EQ(error->kind, ClientErrorKind::server_error) << where << ": " << error->message;
            EXPECT_EQ(error->code, std::optional<int>(code)) << where;
            EXPECT_EQ(error->message, message_of(one.kind)) << where;
            EXPECT_EQ(error->data, data_of(one.kind, code)) << where;
        }
        if (!one.carries_code) continue;
        for (const int code : refused) {
            const std::optional<ClientError> error = one.call(client, code);
            const std::string where = label + " code " + std::to_string(code);
            ASSERT_TRUE(error.has_value()) << where << ": the call did not fail";
            EXPECT_EQ(error->kind, ClientErrorKind::server_error) << where;
            EXPECT_EQ(error->code, std::optional<int>(neomcp::error_code::internal_error)) << where;
            EXPECT_EQ(error->message,
                      std::string(one.kind == kind_tool || one.kind == kind_tool_coroutine ? tool_failed : call_failed))
                << where;
            EXPECT_TRUE(error->data.is_null()) << where << ": " << error->data.dump();
        }
    }
    // Nothing the handlers did closed the connection or made the server forget it.
    EXPECT_TRUE(client.ping() || !neomcp::profile::builtin(revision)->request("ping")) << revision;
}

std::string test_name(std::string text) {
    for (char& c : text) {
        if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    }
    return text;
}

}  // namespace application_errors
