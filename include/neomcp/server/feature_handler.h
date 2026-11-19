// How Server reads the handlers of resources, resource templates, prompts and completers: their form from their
// signature, the result they return, and the type-erased body that the server runs. Everything here is detail of
// neomcp/server.h; the tool handlers have the same rules and live in handler.h.
//
// A handler is a function, a function pointer, or an object with exactly one operator() that is not a template. Its
// parameters say what it takes, in this order: the inputs of its kind, then optionally `Context&`. Its return type
// says whether it is a coroutine (asio::awaitable<R>, with the default executor). Anything else fails to compile with
// a static_assert that says what is expected.
//
//   registered with                         inputs                                            result R
//   resource(uri, name, ...)                 none                                              see below
//   resource_template<Params>(...)           `const Params&` or `Params`                       see below
//   prompt(name, description, ...)           none                                              see below
//   prompt<Args>(...)                        `const Args&` or `Args`                           see below
//   .complete(argument, ...)                 the text typed so far (`std::string_view`,        std::vector<std::string>
//                                            `const std::string&` or `std::string`), and       or neomcp::CompletionResult
//                                            optionally the arguments already resolved
//                                            (`const std::map<std::string, std::string>&`)
//
// The result of a resource or template handler becomes the contents of the read:
//
//   std::string                       text
//   neomcp::Bytes                     a blob (Base64 on the wire)
//   a described struct, neomcp::json  JSON text, with the MIME type application/json unless the resource has one
//   neomcp::ResourceContents          as built (the handler writes the uri itself)
//   std::vector<ResourceContents>     as built
//   neomcp::ReadResult                as built
//
// The result of a prompt handler becomes the messages of the prompt:
//
//   std::string                       one message of the user, with that text
//   neomcp::PromptMessage             one message, as built
//   std::vector<PromptMessage>        the messages, as built
//   neomcp::PromptResult              as built (the description of the prompt's result too)
//
// A resource, template or prompt handler that may ask its client for input returns std::variant<R, InputRequired>
// (or InputRequired alone) for one of the results R above, or is registered with declared inputs and takes their
// answers after its inputs (neomcp/server/inputs.h). A completer never asks.
#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio/awaitable.hpp>

#include "neomcp/base64.h"
#include "neomcp/describe.h"
#include "neomcp/json.h"
#include "neomcp/jsonrpc.h"
#include "neomcp/model.h"
#include "neomcp/server/context.h"
#include "neomcp/server/handler.h"

namespace neomcp::detail {

// ---------------------------------------------------------------------------------------------------------------
// Bodies: what the server runs, with the handler's form erased.

// What a call hands to a body.
struct Invocation {
    const json& arguments;                  // the variables a template matched, as an object
    const std::string& subject;             // the uri read
    const std::optional<std::string>& mime;  // the MIME type of the resource or template read
};

// What a body produces: its kind's result, or a request for input.
template <class Out>
using Produced = std::variant<Out, InputRequired>;

template <class Out>
class FeaturePlainBody {
public:
    virtual ~FeaturePlainBody() = default;
    virtual Produced<Out> run(const Invocation& invocation, Context& context) = 0;
};

template <class Out>
class FeatureCoroutineBody {
public:
    virtual ~FeatureCoroutineBody() = default;
    virtual asio::awaitable<Produced<Out>> run(const Invocation& invocation, Context& context) = 0;
};

// Whether a kind takes R as a handler's result: one of its results, or, for a kind that may ask, InputRequired or
// std::variant of one of its results and InputRequired.
template <class Kind, class R>
constexpr bool kind_accepts() {
    if constexpr (AskingResult<R>::only) {
        return Kind::may_ask;
    } else if constexpr (AskingResult<R>::asks) {
        using T = plain_result_t<R>;
        return Kind::may_ask && !std::is_void_v<T> && !std::is_same_v<T, InputRequired> &&
               Kind::template valid_result<T>();
    } else {
        return Kind::template valid_result<R>();
    }
}

// The outcome of a value a handler returned: its kind's result, or the request for input it made.
template <class Kind, class R>
Produced<typename Kind::Out> produce(R value, const Invocation& invocation) {
    using P = Produced<typename Kind::Out>;
    if constexpr (AskingResult<R>::only) {
        return P(std::in_place_index<1>, std::move(value));
    } else if constexpr (AskingResult<R>::asks) {
        if (auto* asked = std::get_if<InputRequired>(&value)) return P(std::in_place_index<1>, std::move(*asked));
        return P(std::in_place_index<0>, Kind::convert(std::get<0>(std::move(value)), invocation));
    } else {
        return P(std::in_place_index<0>, Kind::convert(std::move(value), invocation));
    }
}

template <class Out>
using FeatureBody = std::variant<std::unique_ptr<FeaturePlainBody<Out>>, std::unique_ptr<FeatureCoroutineBody<Out>>>;

// ---------------------------------------------------------------------------------------------------------------
// What a handler can take

// The inputs a body has decoded for one call.
template <class A>
struct Inputs {
    A args;
};

// How a kind sees one parameter of a handler: 0 when it is no input of the kind, 1 for its main input (the arguments, the
// text typed so far) and 2 for its secondary input (the arguments already resolved).
template <class Kind, class P>
inline constexpr int input_slot_v = Kind::template input_slot<P>();

template <class Kind, class A, class P>
decltype(auto) take(Inputs<A>& inputs, Context& context) {
    if constexpr (is_context_param_v<P>) {
        return (context);
    } else {
        return Kind::template take_input<P>(inputs);
    }
}

template <class Kind, class A, class F, std::size_t... I>
std::remove_cv_t<typename CallSignature<F>::result> call_with_inputs(F& handler, Inputs<A>& inputs, Context& context,
                                                                     std::index_sequence<I...>) {
    using Params = typename CallSignature<F>::params;
    return handler(take<Kind, A, std::tuple_element_t<I, Params>>(inputs, context)...);
}

// Which registration a handler belongs to; it decides what the compile-time messages say.
enum class KindId { resource, resource_template, prompt, prompt_no_arguments, completer };

// What is wrong with the parameters of a handler of the kind, if anything.
enum class ParamProblem { none, unknown, context_not_last, args_missing, args_twice };

template <class Kind, class Params, std::size_t... I>
constexpr ParamProblem param_problem(std::index_sequence<I...>) {
    constexpr std::size_t arity = sizeof...(I);
    constexpr std::size_t args = (std::size_t{0} + ... + (input_slot_v<Kind, std::tuple_element_t<I, Params>> == 1 ? 1u : 0u));
    constexpr std::size_t secondary =
        (std::size_t{0} + ... + (input_slot_v<Kind, std::tuple_element_t<I, Params>> == 2 ? 1u : 0u));
    constexpr bool unknown = (false || ... ||
                              (!is_context_param_v<std::tuple_element_t<I, Params>> &&
                               input_slot_v<Kind, std::tuple_element_t<I, Params>> == 0));
    constexpr bool context_not_last =
        (false || ... || (is_context_param_v<std::tuple_element_t<I, Params>> && I + 1 != arity));
    if constexpr (unknown) {
        return ParamProblem::unknown;
    } else if constexpr (context_not_last) {
        return ParamProblem::context_not_last;
    } else if constexpr (args > 1 || secondary > 1) {
        return ParamProblem::args_twice;
    } else if constexpr (Kind::takes_args && args == 0) {
        return ParamProblem::args_missing;
    } else {
        return ParamProblem::none;
    }
}

// Checks a handler at compile time: the first problem found is reported, and nothing after it is instantiated.
template <class Kind, class F>
constexpr bool check_feature_handler() {
    using Sig = CallSignature<F>;
    if constexpr (!Sig::known) {
        static_assert(dependent_false<F>,
                      "neomcp: a resource, prompt or completion handler must be a function, a function pointer, or an "
                      "object with exactly one operator() that is not a template (write out the parameter types of a "
                      "lambda instead of auto)");
        return false;
    } else if constexpr (std::is_reference_v<typename Sig::result>) {
        static_assert(dependent_false<F>, "neomcp: a handler returns its result by value, not by reference");
        return false;
    } else {
        using Returned = std::remove_cv_t<typename Sig::result>;
        using R = std::remove_cv_t<typename AwaitableTraits<Returned>::value_type>;
        constexpr ParamProblem problem =
            param_problem<Kind, typename Sig::params>(std::make_index_sequence<Sig::arity>{});
        if constexpr (problem == ParamProblem::unknown) {
            if constexpr (Kind::id == KindId::resource) {
                static_assert(dependent_false<F>,
                              "neomcp: a handler registered with server.resource(...) takes no parameters or only "
                              "`neomcp::Context&`");
            } else if constexpr (Kind::id == KindId::prompt_no_arguments) {
                static_assert(dependent_false<F>,
                              "neomcp: a handler registered with server.prompt(...) takes no parameters or only "
                              "`neomcp::Context&`; register a handler that takes arguments with server.prompt<Args>(...)");
            } else if constexpr (Kind::id == KindId::prompt) {
                static_assert(dependent_false<F>,
                              "neomcp: a handler registered with server.prompt<Args>(...) takes the arguments as "
                              "`const Args&` or `Args`, and optionally `neomcp::Context&` after them");
            } else if constexpr (Kind::id == KindId::completer) {
                static_assert(dependent_false<F>,
                              "neomcp: a completion handler takes the text typed so far (`std::string_view`, `const "
                              "std::string&` or `std::string`), optionally the arguments already resolved (`const "
                              "std::map<std::string, std::string>&`), and optionally `neomcp::Context&` last");
            } else {
                static_assert(dependent_false<F>,
                              "neomcp: a handler registered with server.resource_template<Params>(...) takes the "
                              "parameters as `const Params&` or `Params`, and optionally `neomcp::Context&` after them");
            }
            return false;
        } else if constexpr (problem == ParamProblem::context_not_last) {
            static_assert(dependent_false<F>, "neomcp: `neomcp::Context&` is the last parameter of a handler");
            return false;
        } else if constexpr (problem == ParamProblem::args_missing) {
            if constexpr (Kind::id == KindId::prompt) {
                static_assert(dependent_false<F>,
                              "neomcp: a handler registered with server.prompt<Args>(...) takes the arguments as "
                              "`const Args&` or `Args`");
            } else if constexpr (Kind::id == KindId::completer) {
                static_assert(dependent_false<F>,
                              "neomcp: a completion handler takes the text typed so far (`std::string_view`, `const "
                              "std::string&` or `std::string`)");
            } else {
                static_assert(dependent_false<F>,
                              "neomcp: a handler registered with server.resource_template<Params>(...) takes the "
                              "parameters as `const Params&` or `Params`");
            }
            return false;
        } else if constexpr (problem == ParamProblem::args_twice) {
            static_assert(dependent_false<F>, "neomcp: a handler takes its parameters once");
            return false;
        } else if constexpr (!AwaitableTraits<Returned>::default_executor) {
            static_assert(dependent_false<F>,
                          "neomcp: a coroutine handler returns asio::awaitable<R> with the default executor");
            return false;
        } else if constexpr (is_c_string_v<R>) {
            static_assert(dependent_false<F>, "neomcp: a handler returns std::string, not a C string");
            return false;
        } else if constexpr (std::is_reference_v<R> || !kind_accepts<Kind, R>()) {
            if constexpr (Kind::id == KindId::prompt || Kind::id == KindId::prompt_no_arguments) {
                static_assert(dependent_false<F>,
                              "neomcp: a prompt handler returns std::string, neomcp::PromptMessage, "
                              "std::vector<neomcp::PromptMessage> or neomcp::PromptResult, std::variant<R, "
                              "neomcp::InputRequired> of one of them, or neomcp::InputRequired, or an asio::awaitable "
                              "of one of them");
            } else if constexpr (Kind::id == KindId::completer) {
                static_assert(dependent_false<F>,
                              "neomcp: a completion handler returns std::vector<std::string> or "
                              "neomcp::CompletionResult, or an asio::awaitable of one of them");
            } else {
                static_assert(dependent_false<F>,
                              "neomcp: a resource handler returns std::string, neomcp::Bytes, a described struct, "
                              "neomcp::json, neomcp::ResourceContents, std::vector<neomcp::ResourceContents> or "
                              "neomcp::ReadResult, std::variant<R, neomcp::InputRequired> of one of them, or "
                              "neomcp::InputRequired, or an asio::awaitable of one of them");
            }
            return false;
        } else {
            return true;
        }
    }
}

template <class F>
constexpr bool is_coroutine_handler() {
    return AwaitableTraits<std::remove_cv_t<typename CallSignature<F>::result>>::value;
}

// A handler that always throws makes the conversion of its result unreachable in the instantiation for that
// handler, which one compiler reports as a warning in this header; the code is right for every other handler.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4702)
#endif

template <class Kind, class A, class F>
class PlainFeatureBodyOf final : public FeaturePlainBody<typename Kind::Out> {
public:
    template <class H>
    explicit PlainFeatureBodyOf(H&& handler) : handler_(std::forward<H>(handler)) {}

    Produced<typename Kind::Out> run(const Invocation& invocation, Context& context) override {
        Inputs<A> inputs = Kind::template decode<A>(invocation);
        using Sig = CallSignature<F>;
        return produce<Kind>(
            call_with_inputs<Kind, A, F>(handler_, inputs, context, std::make_index_sequence<Sig::arity>{}), invocation);
    }

private:
    F handler_;
};

template <class Kind, class A, class F>
class CoroutineFeatureBodyOf final : public FeatureCoroutineBody<typename Kind::Out> {
public:
    template <class H>
    explicit CoroutineFeatureBodyOf(H&& handler) : handler_(std::forward<H>(handler)) {}

    asio::awaitable<Produced<typename Kind::Out>> run(const Invocation& invocation, Context& context) override {
        // The inputs live in this frame, which outlives the handler's coroutine: a reference parameter is safe.
        Inputs<A> inputs = Kind::template decode<A>(invocation);
        using Sig = CallSignature<F>;
        auto started = call_with_inputs<Kind, A, F>(handler_, inputs, context, std::make_index_sequence<Sig::arity>{});
        using R = typename AwaitableTraits<std::remove_cv_t<typename Sig::result>>::value_type;
        R value = co_await std::move(started);
        co_return produce<Kind>(std::move(value), invocation);
    }

private:
    F handler_;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

template <class Kind, class A, class F, class H>
FeatureBody<typename Kind::Out> make_feature_body(H&& handler) {
    if constexpr (is_coroutine_handler<F>()) {
        return FeatureBody<typename Kind::Out>(
            std::in_place_index<1>, std::make_unique<CoroutineFeatureBodyOf<Kind, A, F>>(std::forward<H>(handler)));
    } else {
        return FeatureBody<typename Kind::Out>(
            std::in_place_index<0>, std::make_unique<PlainFeatureBodyOf<Kind, A, F>>(std::forward<H>(handler)));
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Resources

inline constexpr std::string_view json_mime_type = "application/json";

template <class R>
struct is_resource_contents_vector : std::false_type {};
template <>
struct is_resource_contents_vector<std::vector<ResourceContents>> : std::true_type {};

template <class R>
constexpr bool is_resource_result_type() {
    return std::is_same_v<R, std::string> || std::is_same_v<R, Bytes> || std::is_same_v<R, json> ||
           std::is_same_v<R, ResourceContents> || is_resource_contents_vector<R>::value ||
           std::is_same_v<R, ReadResult> || described<R>;
}

inline ReadResult text_read_result(const Invocation& invocation, std::string text,
                                   const std::optional<std::string>& mime) {
    TextResourceContents contents;
    contents.uri = invocation.subject;
    contents.mime_type = mime;
    contents.text = std::move(text);
    ReadResult result;
    result.contents.emplace_back(std::move(contents));
    return result;
}

template <class R>
ReadResult to_read_result(R value, const Invocation& invocation) {
    if constexpr (std::is_same_v<R, ReadResult>) {
        return value;
    } else if constexpr (std::is_same_v<R, std::string>) {
        return text_read_result(invocation, std::move(value), invocation.mime);
    } else if constexpr (std::is_same_v<R, Bytes>) {
        BlobResourceContents contents;
        contents.uri = invocation.subject;
        contents.mime_type = invocation.mime;
        contents.blob = base64_encode(std::span<const std::byte>(value));
        ReadResult result;
        result.contents.emplace_back(std::move(contents));
        return result;
    } else if constexpr (std::is_same_v<R, json>) {
        return text_read_result(invocation, serialize(value),
                                invocation.mime ? invocation.mime : std::optional<std::string>(std::string(json_mime_type)));
    } else if constexpr (std::is_same_v<R, ResourceContents>) {
        ReadResult result;
        result.contents.push_back(std::move(value));
        return result;
    } else if constexpr (is_resource_contents_vector<R>::value) {
        ReadResult result;
        result.contents = std::move(value);
        return result;
    } else {
        static_assert(described<R>);
        return text_read_result(invocation, serialize(neomcp::to_json(value)),
                                invocation.mime ? invocation.mime : std::optional<std::string>(std::string(json_mime_type)));
    }
}

// The kind of resource() handlers.
struct ResourceKind {
    using Out = ReadResult;
    using Args = NoArguments;
    static constexpr KindId id = KindId::resource;
    static constexpr bool takes_args = false;
    static constexpr bool may_ask = true;
    template <class P>
    static constexpr int input_slot() {
        return 0;
    }

    template <class R>
    static constexpr bool valid_result() {
        return is_resource_result_type<R>();
    }
    template <class A>
    static Inputs<A> decode(const Invocation&) {
        return Inputs<A>{};
    }
    template <class R>
    static Out convert(R value, const Invocation& invocation) {
        return to_read_result<R>(std::move(value), invocation);
    }
};

// The kind of resource_template<Params>() handlers: the matched variables, decoded as the described struct.
template <class Params>
struct TemplateKind {
    using Out = ReadResult;
    using Args = Params;
    static constexpr KindId id = KindId::resource_template;
    static constexpr bool takes_args = true;
    static constexpr bool may_ask = true;
    template <class P>
    static constexpr int input_slot() {
        return is_arguments_param_v<P, Params> ? 1 : 0;
    }
    template <class P, class A>
    static decltype(auto) take_input(Inputs<A>& inputs) {
        return pass_arguments<P>(inputs.args);
    }

    template <class R>
    static constexpr bool valid_result() {
        return is_resource_result_type<R>();
    }
    template <class A>
    static Inputs<A> decode(const Invocation& invocation) {
        return Inputs<A>{neomcp::from_json<Params>(invocation.arguments)};
    }
    template <class R>
    static Out convert(R value, const Invocation& invocation) {
        return to_read_result<R>(std::move(value), invocation);
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Prompts

template <class R>
struct is_prompt_message_vector : std::false_type {};
template <>
struct is_prompt_message_vector<std::vector<PromptMessage>> : std::true_type {};

template <class R>
constexpr bool is_prompt_result_type() {
    return std::is_same_v<R, std::string> || std::is_same_v<R, PromptMessage> || is_prompt_message_vector<R>::value ||
           std::is_same_v<R, PromptResult>;
}

template <class R>
PromptResult to_prompt_result(R value) {
    if constexpr (std::is_same_v<R, PromptResult>) {
        return value;
    } else if constexpr (std::is_same_v<R, std::string>) {
        PromptMessage message;
        message.role = Role::user;
        TextContent text;
        text.text = std::move(value);
        message.content = std::move(text);
        PromptResult result;
        result.messages.push_back(std::move(message));
        return result;
    } else if constexpr (std::is_same_v<R, PromptMessage>) {
        PromptResult result;
        result.messages.push_back(std::move(value));
        return result;
    } else {
        PromptResult result;
        result.messages = std::move(value);
        return result;
    }
}

// The kind of prompt<Args>() handlers: the arguments the request carried, decoded as the described struct.
template <class Parameters>
struct PromptKind {
    using Out = PromptResult;
    using Args = Parameters;
    static constexpr KindId id = KindId::prompt;
    static constexpr bool takes_args = true;
    static constexpr bool may_ask = true;
    template <class P>
    static constexpr int input_slot() {
        return is_arguments_param_v<P, Parameters> ? 1 : 0;
    }
    template <class P, class A>
    static decltype(auto) take_input(Inputs<A>& inputs) {
        return pass_arguments<P>(inputs.args);
    }

    template <class R>
    static constexpr bool valid_result() {
        return is_prompt_result_type<R>();
    }
    template <class A>
    static Inputs<A> decode(const Invocation& invocation) {
        return Inputs<A>{neomcp::from_json<Parameters>(invocation.arguments)};
    }
    template <class R>
    static Out convert(R value, const Invocation&) {
        return to_prompt_result<R>(std::move(value));
    }
};

// The kind of prompt() handlers.
struct PromptNoArgumentsKind {
    using Out = PromptResult;
    using Args = NoArguments;
    static constexpr KindId id = KindId::prompt_no_arguments;
    static constexpr bool takes_args = false;
    static constexpr bool may_ask = true;
    template <class P>
    static constexpr int input_slot() {
        return 0;
    }

    template <class R>
    static constexpr bool valid_result() {
        return is_prompt_result_type<R>();
    }
    template <class A>
    static Inputs<A> decode(const Invocation&) {
        return Inputs<A>{};
    }
    template <class R>
    static Out convert(R value, const Invocation&) {
        return to_prompt_result<R>(std::move(value));
    }
};

template <class M>
constexpr bool is_prompt_argument_type() {
    return std::is_same_v<M, std::string> || std::is_same_v<M, std::optional<std::string>>;
}

template <class Args>
constexpr bool all_prompt_arguments_are_strings() {
    return std::apply(
        [](const auto&... field) {
            return (is_prompt_argument_type<typename std::decay_t<decltype(field)>::member_type>() && ...);
        },
        Described<Args>::fields.items);
}

// The argument list of a prompt: its members, a member that is not std::optional being required.
template <class Args>
std::vector<PromptArgumentDefinition> prompt_arguments_of() {
    std::vector<PromptArgumentDefinition> arguments;
    std::apply(
        [&](const auto&... field) {
            (arguments.push_back([&] {
                PromptArgumentDefinition argument;
                argument.name = std::string(field.name);
                if (!field.description.empty()) argument.description = std::string(field.description);
                argument.required = !is_optional_v<typename std::decay_t<decltype(field)>::member_type>;
                return argument;
            }()),
             ...);
        },
        Described<Args>::fields.items);
    return arguments;
}

// ---------------------------------------------------------------------------------------------------------------
// Completion

template <class R>
constexpr bool is_completion_result_type() {
    return std::is_same_v<R, std::vector<std::string>> || std::is_same_v<R, CompletionResult>;
}

using ResolvedArguments = std::map<std::string, std::string>;

template <class P>
inline constexpr bool is_text_param_v =
    std::is_same_v<P, std::string_view> || std::is_same_v<P, const std::string&> || std::is_same_v<P, std::string>;

template <class P>
inline constexpr bool is_resolved_param_v =
    std::is_same_v<P, const ResolvedArguments&> || std::is_same_v<P, ResolvedArguments>;

// What a completion handler is given.
struct CompleterInput {
    std::string text;
    ResolvedArguments resolved;
};

// The kind of `.complete(argument, handler)` handlers. The call's arguments are `{"value": text, "arguments": {...}}`.
struct CompleterKind {
    using Out = CompletionResult;
    using Args = CompleterInput;
    static constexpr KindId id = KindId::completer;
    static constexpr bool takes_args = true;
    static constexpr bool may_ask = false;

    template <class P>
    static constexpr int input_slot() {
        return is_text_param_v<P> ? 1 : (is_resolved_param_v<P> ? 2 : 0);
    }
    template <class P, class A>
    static decltype(auto) take_input(Inputs<A>& inputs) {
        if constexpr (std::is_same_v<P, std::string_view>) {
            return std::string_view(inputs.args.text);
        } else if constexpr (std::is_same_v<P, const std::string&>) {
            return static_cast<const std::string&>(inputs.args.text);
        } else if constexpr (std::is_same_v<P, std::string>) {
            return std::string(inputs.args.text);
        } else if constexpr (std::is_same_v<P, const ResolvedArguments&>) {
            return static_cast<const ResolvedArguments&>(inputs.args.resolved);
        } else {
            return ResolvedArguments(inputs.args.resolved);
        }
    }

    template <class R>
    static constexpr bool valid_result() {
        return is_completion_result_type<R>();
    }
    template <class A>
    static Inputs<A> decode(const Invocation& invocation) {
        Inputs<A> inputs;
        if (const auto* value = invocation.arguments.get_ptr<const json::object_t*>()) {
            const auto text = value->find("value");
            if (text != value->end() && text->second.is_string()) inputs.args.text = text->second.template get<std::string>();
            const auto resolved = value->find("arguments");
            if (resolved != value->end()) {
                if (const auto* entries = resolved->second.template get_ptr<const json::object_t*>()) {
                    for (const auto& [name, entry] : *entries) {
                        if (entry.is_string()) inputs.args.resolved.emplace(name, entry.template get<std::string>());
                    }
                }
            }
        }
        return inputs;
    }
    template <class R>
    static Out convert(R value, const Invocation&) {
        if constexpr (std::is_same_v<R, CompletionResult>) {
            return value;
        } else {
            CompletionResult result;
            result.values = std::move(value);
            return result;
        }
    }
};

// ---------------------------------------------------------------------------------------------------------------
// The variables of a resource template

// How a variable's text becomes a member of the parameters struct.
enum class VariableKind { text, signed_integer, unsigned_integer, boolean };

struct TemplateMember {
    std::string name;
    VariableKind kind = VariableKind::text;
};

template <class M>
constexpr bool is_template_member_type() {
    using T = optional_core_t<M>;
    return std::is_same_v<T, std::string> || std::is_same_v<T, bool> ||
           (std::is_integral_v<T> && !std::is_same_v<T, char> && !std::is_same_v<T, wchar_t> &&
            !std::is_same_v<T, char8_t> && !std::is_same_v<T, char16_t> && !std::is_same_v<T, char32_t>);
}

template <class M>
constexpr VariableKind variable_kind_of() {
    using T = optional_core_t<M>;
    if constexpr (std::is_same_v<T, bool>) {
        return VariableKind::boolean;
    } else if constexpr (std::is_same_v<T, std::string>) {
        return VariableKind::text;
    } else if constexpr (std::is_signed_v<T>) {
        return VariableKind::signed_integer;
    } else {
        return VariableKind::unsigned_integer;
    }
}

template <class Params>
constexpr bool all_template_members_decodable() {
    return std::apply([](const auto&... field) { return (is_template_member_type<typename std::decay_t<decltype(field)>::member_type>() && ...); },
                      Described<Params>::fields.items);
}

// The members of the parameters struct, by their JSON names.
template <class Params>
std::vector<TemplateMember> template_members_of() {
    std::vector<TemplateMember> members;
    std::apply(
        [&](const auto&... field) {
            (members.push_back(TemplateMember{std::string(field.name),
                                              variable_kind_of<typename std::decay_t<decltype(field)>::member_type>()}),
             ...);
        },
        Described<Params>::fields.items);
    return members;
}

// Whether the variables of a match, written as an object, decode as the parameters struct.
using TemplateCheck = std::function<bool(const json&)>;

// The JSON value a variable's text stands for; empty when it cannot be one of the member's kind.
std::optional<json> variable_value(VariableKind kind, const std::string& text);

}  // namespace neomcp::detail
