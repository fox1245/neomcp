// What the typed methods of neomcp::Client give back: pages of a list, whole lists, tool results with the check of
// their structured content, and the decoded value of a typed call.
//
//   auto tools = client->list_all_tools();
//   auto sum = client->call<int>("add", AddArgs{1, 2});
//   if (sum && !sum->is_error) use(*sum->value);
//
// The check of a tool's structured content. A tool that declares an output schema promises that its structured content
// fits it, and a client should hold it to that promise. The client keeps the definition of every tool the server
// listed (the latest one under each name). When a result with structured content arrives for such a tool, the output
// schema is compiled once, within the peer bounds of ClientOptions and against the client's budget of compiles of the
// server's schemas, kept with that definition, and the content is validated against it. A result that is a tool error
// is not checked. call_tool() gives the outcome as data and never fails because of it; call<R>() fails unless the
// content passed (or the tool declares no output schema), and then decodes it as R.
#pragma once

#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/awaitable.hpp>

#include "neomcp/client/result.h"
#include "neomcp/describe.h"
#include "neomcp/json.h"
#include "neomcp/json_schema.h"
#include "neomcp/model.h"

namespace neomcp {

// ---- Lists

// One page of a list, as the server sent it.
template <class T>
struct Page {
    std::vector<T> items;
    // The cursor of the next page, exactly as the server sent it (an empty string is a cursor too); empty: this was the
    // last page. Pass it to the same list method to get the next page.
    std::optional<std::string> next_cursor;
    // The page's caching hints, where the revision has them.
    std::optional<CacheHints> cache;
};

// Every page of a list, in order.
template <class T>
struct Listing {
    std::vector<T> items;
    // The caching hints of each page, in the order of the pages (one entry per page, empty where the page had none).
    // They are not merged: a client that keeps the list keeps it no longer than its shortest-lived page allows.
    std::vector<std::optional<CacheHints>> page_cache;
};

// ---- Tool results

enum class OutputCheckKind {
    not_applicable,  // nothing to check: the result is a tool error, or it carries no structured content
    not_listed,      // the tool is not among the tools the server listed, so its output schema is not known
    no_schema,       // the tool's definition declares no output schema
    valid,           // the structured content fits the output schema
    invalid,         // it does not (`issue` says where); a bound the validator met while checking reads the same
    unverified       // the output schema could not be compiled: refused within the peer bounds (`issue`), or the
                     // budget of compiles was spent (`budget_spent`); the content is taken as not checked
};

struct OutputCheck {
    OutputCheckKind kind = OutputCheckKind::not_applicable;
    // invalid: where the content fails (pointers into the content and into the schema); unverified: why the schema was
    // refused. The message may quote the schema, which the server wrote.
    std::optional<json_schema::Issue> issue;
    // unverified: the budget of compiles (ClientOptions::max_peer_schema_compiles) was spent before this schema.
    bool budget_spent = false;
};

// The result of call_tool(): the tool's result, read for the governing revision, with the check of its structured
// content.
struct ToolCall : ToolResult {
    OutputCheck check;
};

// The result of call<R>(): the tool's result with its check, and the structured content decoded as R. `value` is
// empty exactly when the tool reported a failure of its own (`is_error`), which is a value and not a ClientError.
template <class R>
struct TypedCall : ToolCall {
    std::optional<R> value;
};

// ---- Completion

// What a completion refers to: a prompt by its name, or a resource template by its URI template.
struct CompletionRef {
    enum class Kind { prompt, resource };
    Kind kind = Kind::prompt;
    std::string reference;

    static CompletionRef prompt(std::string name) { return CompletionRef{Kind::prompt, std::move(name)}; }
    static CompletionRef resource(std::string uri) { return CompletionRef{Kind::resource, std::move(uri)}; }
};

namespace detail {

// The member of structured content that holds a result that is not an object: `{"result": value}`. A server built with
// this library writes such results so, with an output schema of that object.
inline constexpr std::string_view typed_result_member = "result";

// Whether a typed call decodes the whole structured content as R (a described struct, or neomcp::json), rather than
// its typed_result_member.
template <class R>
constexpr bool typed_result_whole() {
    constexpr Kind kind = kind_of<R>();
    return kind == Kind::object || kind == Kind::any_json;
}

// Whether a typed call can give back an R: a described struct, neomcp::json, or what a server wraps (bool, a number, a
// described enum, a vector or a string-keyed map of such values). A std::string result is text content, which has no
// structured form: read it with call_tool().
template <class R>
constexpr bool typed_result_supported() {
    constexpr Kind kind = kind_of<R>();
    if constexpr (kind == Kind::object || kind == Kind::any_json || kind == Kind::boolean || kind == Kind::integer ||
                  kind == Kind::floating || kind == Kind::enumeration) {
        return true;
    } else if constexpr (kind == Kind::array || kind == Kind::dictionary) {
        return has_schema<R>();
    } else {
        return false;
    }
}

inline ClientError typed_error(ClientErrorKind kind, std::string message) {
    ClientError error;
    error.kind = kind;
    error.message = std::move(message);
    return error;
}

// A tool call with its check, decoded as R; see Client::call.
template <class R>
Result<TypedCall<R>> decode_typed(Result<ToolCall> called) {
    if (!called) return std::move(called).error();
    TypedCall<R> typed;
    static_cast<ToolCall&>(typed) = std::move(called).value();
    if (typed.is_error) return typed;
    switch (typed.check.kind) {
        case OutputCheckKind::invalid:
            return typed_error(ClientErrorKind::output_invalid,
                               "the tool's structured content does not fit the tool's output schema");
        case OutputCheckKind::unverified:
            return typed_error(ClientErrorKind::unverified,
                               typed.check.budget_spent
                                   ? "the tool's output schema was not compiled: the budget of schema compiles is spent"
                                   : "the tool's output schema cannot be compiled within the limits for a server's "
                                     "schemas");
        case OutputCheckKind::not_listed:
            return typed_error(ClientErrorKind::unverified,
                               "the tool is not in the server's list of tools, so its output schema is not known");
        case OutputCheckKind::not_applicable:
            return typed_error(ClientErrorKind::output_undecodable, "the tool's result carries no structured content");
        case OutputCheckKind::no_schema:
        case OutputCheckKind::valid:
            break;
    }
    const json& content = *typed.structured_content;
    const json* source = &content;
    if constexpr (!typed_result_whole<R>()) {
        source = nullptr;
        if (const auto* members = content.get_ptr<const json::object_t*>()) {
            const auto found = members->find(std::string(typed_result_member));
            if (found != members->end()) source = &found->second;
        }
        if (source == nullptr) {
            return typed_error(ClientErrorKind::output_undecodable,
                               "the tool's structured content has no member \"" + std::string(typed_result_member) +
                                   "\" to decode");
        }
    }
    try {
        typed.value.emplace(from_json<R>(*source));
    } catch (const std::exception&) {
        // DecodeError, or what a conversion of the application's own threw.
        return typed_error(ClientErrorKind::output_undecodable,
                           "the tool's structured content does not have the shape of the result type");
    }
    return typed;
}

template <class R>
asio::awaitable<Result<TypedCall<R>>> decode_typed_async(asio::awaitable<Result<ToolCall>> call) {
    Result<ToolCall> called = co_await std::move(call);
    Result<TypedCall<R>> typed = decode_typed<R>(std::move(called));
    co_return typed;
}

}  // namespace detail

}  // namespace neomcp
