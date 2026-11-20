// How Server reads a tool handler: its form from its signature, the result it returns, and the type-erased body
// that the server runs. Everything here is detail of neomcp/server.h.
//
// A handler is a function, a function pointer, or an object with exactly one operator() that is not a template
// (a lambda whose parameters are written out). Its parameters say what it takes and its return type says whether
// it is a coroutine:
//
//   registered with           plain function               coroutine
//   tool<Args>(...)           R(const Args&)               asio::awaitable<R>(Args)
//                             R(const Args&, Context&)     asio::awaitable<R>(Args, Context&)
//   tool(name, desc, ...)     R()                          asio::awaitable<R>()
//                             R(Context&)                  asio::awaitable<R>(Context&)
//   tool(ToolDefinition, ...) ToolResult(const json&, Context&)   asio::awaitable<ToolResult>(json, Context&)
//
// The arguments may be taken as `const Args&` or as `Args` in either kind. Anything else fails to compile with a
// static_assert that says what is expected.
//
// A handler that may ask its client for input returns std::variant<R, neomcp::InputRequired> (or InputRequired
// alone), where R is one of the results below; a ToolDefinition handler then returns
// std::variant<neomcp::ToolResult, neomcp::InputRequired>. A handler registered with declared inputs takes their
// answers after the arguments instead (neomcp/server/inputs.h).
#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio/awaitable.hpp>

#include "neomcp/describe.h"
#include "neomcp/detail/callable.h"
#include "neomcp/input.h"
#include "neomcp/json.h"
#include "neomcp/jsonrpc.h"
#include "neomcp/model.h"
#include "neomcp/server/context.h"

namespace neomcp::detail {

// ---------------------------------------------------------------------------------------------------------------
// Signatures: the generic reading of a callable is in neomcp/detail/callable.h.

// What a handler takes as its first parameter.
enum class Input {
    typed,    // a described struct (Server::tool<Args>)
    none,     // nothing (Server::tool(name, description, handler))
    untyped,  // the arguments object (Server::tool(ToolDefinition, handler))
};

// The arguments type of a handler that takes none.
struct NoArguments {};

template <class P, class Args>
inline constexpr bool is_arguments_param_v = std::is_same_v<P, Args> || std::is_same_v<P, const Args&>;

template <class P>
inline constexpr bool is_context_param_v = std::is_same_v<P, Context&>;

// The facts of a handler whose signature is known.
template <Input in, class F>
struct Shape {
    using signature = CallSignature<F>;
    using returned = std::remove_cv_t<typename signature::result>;
    static constexpr bool coroutine = AwaitableTraits<returned>::value;
    // The value the handler produces: R of R(...) or of asio::awaitable<R>(...).
    using value = std::remove_cv_t<typename AwaitableTraits<returned>::value_type>;
    using first = param_t<0, typename signature::params>;
    static constexpr std::size_t inputs = in == Input::none ? 0 : 1;
    static constexpr bool takes_context = signature::arity == inputs + 1;
};

// ---------------------------------------------------------------------------------------------------------------
// Results

// A result that may ask for input instead: InputRequired alone, or std::variant<R, InputRequired>; `result` is R.
template <class R>
struct AskingResult {
    static constexpr bool asks = false;
    static constexpr bool only = false;  // InputRequired alone
    using result = R;
};
template <>
struct AskingResult<InputRequired> {
    static constexpr bool asks = true;
    static constexpr bool only = true;
    using result = void;
};
template <class R>
struct AskingResult<std::variant<R, InputRequired>> {
    static constexpr bool asks = true;
    static constexpr bool only = false;
    using result = R;
};

// The result type behind one that may ask.
template <class R>
using plain_result_t = typename AskingResult<R>::result;

// What a tool body produces: the tool's result, or a request for input.
using ToolOutcome = std::variant<ToolResult, InputRequired>;

// The member that holds a value that is not an object in structured content (`{"result": value}`).
inline constexpr std::string_view wrapped_result_member = "result";

// A result that is wrapped: bool, a number, a described enum, a vector or a string-keyed map of such things.
template <class R>
constexpr bool is_wrapped_result() {
    constexpr Kind kind = kind_of<R>();
    if constexpr (kind == Kind::boolean || kind == Kind::integer || kind == Kind::floating ||
                  kind == Kind::enumeration) {
        return true;
    } else if constexpr (kind == Kind::array || kind == Kind::dictionary) {
        return has_schema<R>();
    } else {
        return false;
    }
}

template <class R>
constexpr bool is_plain_tool_result_type() {
    if constexpr (std::is_void_v<R>) {
        return true;
    } else if constexpr (std::is_same_v<R, std::string> || std::is_same_v<R, json> || std::is_same_v<R, ToolResult>) {
        return true;
    } else if constexpr (described<R>) {
        return true;
    } else {
        return is_wrapped_result<R>();
    }
}

template <class R>
constexpr bool is_tool_result_type() {
    if constexpr (AskingResult<R>::only) {
        return true;
    } else if constexpr (AskingResult<R>::asks) {
        using T = plain_result_t<R>;
        return !std::is_void_v<T> && !std::is_same_v<T, InputRequired> && is_plain_tool_result_type<T>();
    } else {
        return is_plain_tool_result_type<R>();
    }
}

inline ContentBlock text_block(std::string text) {
    TextContent block;
    block.text = std::move(text);
    return block;
}

// The output schema a result type declares; empty for results whose shape the type does not fix.
template <class R>
std::optional<json> output_schema_of() {
    if constexpr (AskingResult<R>::only) {
        return std::nullopt;
    } else if constexpr (AskingResult<R>::asks) {
        return output_schema_of<plain_result_t<R>>();
    } else if constexpr (std::is_void_v<R> || std::is_same_v<R, std::string> || std::is_same_v<R, json> ||
                         std::is_same_v<R, ToolResult>) {
        return std::nullopt;
    } else if constexpr (described<R>) {
        return schema_of<R>();
    } else {
        json properties = json::object();
        properties[std::string(wrapped_result_member)] = schema_of<R>();
        json required = json::array();
        required.push_back(std::string(wrapped_result_member));
        json schema = json::object();
        schema["type"] = "object";
        schema["properties"] = std::move(properties);
        schema["required"] = std::move(required);
        schema["additionalProperties"] = false;
        return schema;
    }
}

// The tool result of a value a handler returned (the table in neomcp/server.h).
template <class R>
ToolResult to_tool_result(R value) {
    if constexpr (std::is_same_v<R, ToolResult>) {
        return value;
    } else if constexpr (std::is_same_v<R, std::string>) {
        ToolResult result;
        result.content.push_back(text_block(std::move(value)));
        return result;
    } else if constexpr (std::is_same_v<R, json>) {
        ToolResult result;
        result.content.push_back(text_block(serialize(value)));
        if (value.is_object()) result.structured_content = std::move(value);
        return result;
    } else if constexpr (described<R>) {
        json encoded = neomcp::to_json(value);
        ToolResult result;
        result.content.push_back(text_block(serialize(encoded)));
        result.structured_content = std::move(encoded);
        return result;
    } else {
        json encoded = neomcp::to_json(value);
        ToolResult result;
        result.content.push_back(text_block(serialize(encoded)));
        json wrapped = json::object();
        wrapped[std::string(wrapped_result_member)] = std::move(encoded);
        result.structured_content = std::move(wrapped);
        return result;
    }
}

// The outcome of a value a handler returned: its tool result, or the request for input it made.
template <class R>
ToolOutcome to_tool_outcome(R value) {
    if constexpr (AskingResult<R>::only) {
        return ToolOutcome(std::in_place_index<1>, std::move(value));
    } else if constexpr (AskingResult<R>::asks) {
        if (auto* asked = std::get_if<InputRequired>(&value)) return ToolOutcome(std::in_place_index<1>, std::move(*asked));
        return ToolOutcome(std::in_place_index<0>, to_tool_result<plain_result_t<R>>(std::get<0>(std::move(value))));
    } else {
        return ToolOutcome(std::in_place_index<0>, to_tool_result<R>(std::move(value)));
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Checking a handler at compile time: the first problem found is reported, and nothing after it is instantiated.

template <class R>
inline constexpr bool is_c_string_v = std::is_same_v<R, const char*> || std::is_same_v<R, char*>;

template <Input in, class Args, class F>
constexpr bool check_handler() {
    using Sig = CallSignature<F>;
    if constexpr (!Sig::known) {
        static_assert(dependent_false<F>,
                      "neomcp: a tool handler must be a function, a function pointer, or an object with exactly one "
                      "operator() that is not a template (write out the parameter types of a lambda instead of auto)");
        return false;
    } else if constexpr (std::is_reference_v<typename Sig::result>) {
        static_assert(dependent_false<F>, "neomcp: a tool handler returns its result by value, not by reference");
        return false;
    } else {
        using S = Shape<in, F>;
        constexpr std::size_t arity = Sig::arity;
        using P0 = param_t<0, typename Sig::params>;
        using P1 = param_t<1, typename Sig::params>;
        using R = typename S::value;
        if constexpr (in == Input::typed && (arity == 0 || !is_arguments_param_v<P0, Args>)) {
            static_assert(dependent_false<F>,
                          "neomcp: a handler registered with server.tool<Args>(...) takes the arguments as its first "
                          "parameter, as `const Args&` or `Args`");
            return false;
        } else if constexpr (in == Input::untyped && (arity == 0 || !is_arguments_param_v<P0, json>)) {
            static_assert(dependent_false<F>,
                          "neomcp: a handler registered with a ToolDefinition takes the arguments as its first "
                          "parameter, as `const neomcp::json&` or `neomcp::json`");
            return false;
        } else if constexpr (in != Input::none && arity > 2) {
            static_assert(dependent_false<F>,
                          "neomcp: a tool handler takes at most two parameters: the arguments and `neomcp::Context&`");
            return false;
        } else if constexpr (in != Input::none && arity == 2 && !is_context_param_v<P1>) {
            static_assert(dependent_false<F>,
                          "neomcp: the parameter of a tool handler after the arguments is `neomcp::Context&`");
            return false;
        } else if constexpr (in == Input::none && (arity > 1 || (arity == 1 && !is_context_param_v<P0>))) {
            static_assert(dependent_false<F>,
                          "neomcp: a handler registered without an arguments type takes no parameters or only "
                          "`neomcp::Context&`; register a handler that takes arguments with server.tool<Args>(...)");
            return false;
        } else if constexpr (!AwaitableTraits<typename S::returned>::default_executor) {
            static_assert(dependent_false<F>,
                          "neomcp: a coroutine handler returns asio::awaitable<R> with the default executor");
            return false;
        } else if constexpr (in == Input::untyped && !std::is_same_v<R, ToolResult> &&
                             !std::is_same_v<R, std::variant<ToolResult, InputRequired>>) {
            static_assert(dependent_false<F>,
                          "neomcp: a handler registered with a ToolDefinition returns neomcp::ToolResult or "
                          "std::variant<neomcp::ToolResult, neomcp::InputRequired>, or an asio::awaitable of one of them");
            return false;
        } else if constexpr (is_c_string_v<R>) {
            static_assert(dependent_false<F>, "neomcp: a tool handler returns std::string, not a C string");
            return false;
        } else if constexpr (std::is_reference_v<R> || !is_tool_result_type<R>()) {
            static_assert(dependent_false<F>,
                          "neomcp: a tool handler returns std::string, a described struct, bool, a number, "
                          "std::vector<T>, std::map<std::string, T>, a described enum, neomcp::json, "
                          "neomcp::ToolResult or void, std::variant<R, neomcp::InputRequired> of one of them but void, "
                          "or neomcp::InputRequired, or an asio::awaitable of one of them");
            return false;
        } else {
            return true;
        }
    }
}

// The value a checked handler produces.
template <Input in, class F>
using handler_value_t = typename Shape<in, F>::value;

// ---------------------------------------------------------------------------------------------------------------
// Bodies: what the server runs, with the handler's form erased.

// Runs on a worker thread; throws what the handler (or decoding its arguments) throws.
class PlainBody {
public:
    virtual ~PlainBody() = default;
    virtual ToolOutcome run(const json& arguments, Context& context) = 0;
};

// Runs on the server's coroutine thread; the arguments and the context outlive the coroutine.
class CoroutineBody {
public:
    virtual ~CoroutineBody() = default;
    virtual asio::awaitable<ToolOutcome> run(const json& arguments, Context& context) = 0;
};

using Body = std::variant<std::unique_ptr<PlainBody>, std::unique_ptr<CoroutineBody>>;

// The arguments as the handler's first parameter: a reference to them, or them moved into a by-value parameter.
template <class P, class Args>
decltype(auto) pass_arguments(Args& args) {
    if constexpr (std::is_reference_v<P>) {
        return static_cast<const Args&>(args);
    } else {
        return Args(std::move(args));
    }
}

template <class Args>
Args decode_arguments(const json& arguments) {
    if constexpr (std::is_same_v<Args, NoArguments>) {
        static_cast<void>(arguments);
        return Args{};
    } else {
        return neomcp::from_json<Args>(arguments);
    }
}

// Calls the handler with what its form takes and returns what it returns (for a coroutine, the awaitable).
template <Input in, class Args, class F>
typename Shape<in, F>::returned invoke_handler(F& handler, Args& args, Context& context) {
    using S = Shape<in, F>;
    if constexpr (in == Input::none) {
        static_cast<void>(args);
        if constexpr (S::takes_context) {
            return handler(context);
        } else {
            static_cast<void>(context);
            return handler();
        }
    } else if constexpr (S::takes_context) {
        return handler(pass_arguments<typename S::first>(args), context);
    } else {
        static_cast<void>(context);
        return handler(pass_arguments<typename S::first>(args));
    }
}

// A handler that always throws makes the conversion of its result unreachable in the instantiation for that
// handler, which one compiler reports as a warning in this header; the code is right for every other handler.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4702)
#endif

template <Input in, class Args, class F>
class PlainBodyOf final : public PlainBody {
public:
    template <class H>
    explicit PlainBodyOf(H&& handler) : handler_(std::forward<H>(handler)) {}

    ToolOutcome run(const json& arguments, Context& context) override {
        using R = handler_value_t<in, F>;
        Args args = decode_arguments<Args>(arguments);
        if constexpr (std::is_void_v<R>) {
            invoke_handler<in>(handler_, args, context);
            return ToolOutcome(std::in_place_index<0>, ToolResult{});
        } else {
            return to_tool_outcome<R>(invoke_handler<in>(handler_, args, context));
        }
    }

private:
    F handler_;
};

template <Input in, class Args, class F>
class CoroutineBodyOf final : public CoroutineBody {
public:
    template <class H>
    explicit CoroutineBodyOf(H&& handler) : handler_(std::forward<H>(handler)) {}

    asio::awaitable<ToolOutcome> run(const json& arguments, Context& context) override {
        using R = handler_value_t<in, F>;
        // The arguments live in this frame, which outlives the handler's coroutine: a reference parameter is safe.
        Args args = decode_arguments<Args>(arguments);
        auto started = invoke_handler<in>(handler_, args, context);
        if constexpr (std::is_void_v<R>) {
            co_await std::move(started);
            co_return ToolOutcome(std::in_place_index<0>, ToolResult{});
        } else {
            R value = co_await std::move(started);
            co_return to_tool_outcome<R>(std::move(value));
        }
    }

private:
    F handler_;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

template <Input in, class Args, class F, class H>
Body make_body(H&& handler) {
    if constexpr (Shape<in, F>::coroutine) {
        return Body(std::in_place_index<1>, std::make_unique<CoroutineBodyOf<in, Args, F>>(std::forward<H>(handler)));
    } else {
        return Body(std::in_place_index<0>, std::make_unique<PlainBodyOf<in, Args, F>>(std::forward<H>(handler)));
    }
}

// The input schema of a tool without parameters: an object with no members.
inline json empty_object_schema() {
    json schema = json::object();
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    return schema;
}

}  // namespace neomcp::detail
