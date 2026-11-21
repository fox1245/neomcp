// Declared inputs: what a tool, a prompt, a resource or a resource template asks its client for before its handler
// runs, declared after the handler instead of written by hand as code that is entered again (neomcp::InputRequired).
//
//   struct Confirm { bool ok; };
//   NEOMCP_DESCRIBE(Confirm, ok, "Delete it?")
//
//   server.tool<DeleteArgs>("delete_folder", "Delete a folder",
//       [](const DeleteArgs& a, const neomcp::Answer<Confirm>& confirm) {
//           if (!confirm.accepted() || !confirm->ok) return std::string("kept");
//           remove_folder(a.path);
//           return std::string("deleted");
//       },
//       neomcp::elicit<Confirm>("confirm", [](const DeleteArgs& a) { return "Delete " + a.path + "?"; }));
//
// An input has a key (unique among the inputs of its handler), a kind and a question function:
//
//   neomcp::elicit<Form>(key, question)   a form the user fills in, answered as the described struct Form. The form
//                                         is form_schema_of<Form>() (neomcp/server/form_schema.h); the question
//                                         returns the message shown with it.
//   neomcp::sample(key, question)         a completion from the client's model, answered as neomcp::SamplingAnswer;
//                                         the question returns the neomcp::SamplingRequest.
//   neomcp::roots(key)                    the client's roots, answered as neomcp::RootsAnswer; with a question
//   neomcp::roots(key, question)          function that returns neomcp::RootsRequest{} or that no question is needed.
//
// Any of them followed by `.optional()` is optional: a client that cannot take its request (see
// Context::client_supports) is not asked, and the handler gets an answer that was not asked. A required input that the
// client cannot take ends the call as a handler that asks for it would: with the revision's error for a missing client
// capability where it defines one, else as a failed call. Elicitation in URL mode is not declared: after the user
// agreed to visit the URL, the server decides whether to ask again, which a handler that runs once cannot do; a
// handler that needs it returns neomcp::InputRequired itself.
//
// The handler takes, after its arguments (if it has any) and before an optional `neomcp::Context&`, one
// `const neomcp::Answer<T>&` (or `neomcp::Answer<T>`) per input, in the order the inputs are declared. Anything else
// fails to compile with a static_assert that says what is expected; so does a handler with declared inputs whose
// result can be neomcp::InputRequired (its inputs are asked for it, and it does not ask itself).
//
// A question function takes nothing; or the arguments (`const Args&` or `Args`); or the arguments followed by the
// answers of every input declared before it, in order (inputs of a handler without arguments: nothing, or the
// earlier answers). One that takes earlier answers is asked only once each of them has its outcome, so it is asked in
// a later round than they are. It returns the question (the message, the request) or a neomcp::Question<T>, which
// also says that no question is needed: neomcp::no_question(value) gives the handler that value as accepted,
// neomcp::no_question<T>() gives it an answer that was not asked.
//
// What runs when. On every entry of a call (its first, and each round that brings answers) the question functions run,
// in the order the inputs are declared, and the inputs whose outcome is still missing are asked together. The handler
// runs exactly once, when every input has its outcome; it is never entered in a round that asks. So a question
// function must have no effects, and must render the same question for the same arguments and earlier answers: an
// answer counts only for the very question that was asked, and a question that changed (one with the time in it, or a
// counter) is asked again.
//
// What the client answers is checked before the handler sees it. A form's content must match its schema and decode
// as Form; an answer of another kind than its input's, or content that does not fit, ends the call with Invalid params.
// Only the last round's answers come with a call; the outcomes of earlier rounds, accepted values included, are kept in
// the call's state, each with a digest of the question it answers. The state is the handler's own (where the result
// asks it travels sealed, see neomcp/server.h); it holds at most ServerOptions::max_declared_state_bytes, and a round
// that would need more ends the call with Invalid params (nothing is dropped). A call that comes back with answers
// and without any state (see ServerOptions::answers_need_state) has no earlier rounds: its answers count for the
// questions as they are now.
//
// Everything below the public declarations is detail of neomcp/server.h.
#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include <asio/awaitable.hpp>

#include "neomcp/describe.h"
#include "neomcp/input.h"
#include "neomcp/json.h"
#include "neomcp/json_schema.h"
#include "neomcp/model.h"
#include "neomcp/server/context.h"
#include "neomcp/server/feature_handler.h"
#include "neomcp/server/form_schema.h"
#include "neomcp/server/handler.h"

namespace neomcp {

// The outcome of one declared input.
enum class AnswerStatus {
    accepted,   // the client answered: the user submitted the form, the model completed, the client listed its roots
    declined,   // the user declined the form
    cancelled,  // the user dismissed the form without choosing
    not_asked   // no question was needed, or the input is optional and the client cannot take its request
};

// What a handler gets for one declared input: its status, and the answer when it was accepted (a Form for a form, a
// SamplingAnswer, a RootsAnswer).
template <class T>
class Answer {
public:
    // Not asked.
    Answer() = default;
    // Accepted, with the answer.
    explicit Answer(T accepted_value) : status_(AnswerStatus::accepted), value_(std::move(accepted_value)) {}
    // Declined, cancelled or not asked; an accepted answer needs its value (std::invalid_argument).
    explicit Answer(AnswerStatus outcome) : status_(outcome) {
        if (outcome == AnswerStatus::accepted) throw std::invalid_argument("neomcp: an accepted answer carries its value");
    }

    AnswerStatus status() const noexcept { return status_; }
    bool accepted() const noexcept { return status_ == AnswerStatus::accepted; }
    bool declined() const noexcept { return status_ == AnswerStatus::declined; }
    bool cancelled() const noexcept { return status_ == AnswerStatus::cancelled; }
    // Whether the client was asked (and answered): every status but not_asked.
    bool asked() const noexcept { return status_ != AnswerStatus::not_asked; }

    // The accepted answer; std::logic_error when it was not accepted.
    const T& value() const {
        if (status_ != AnswerStatus::accepted) throw std::logic_error("neomcp: the input was not accepted");
        return *value_;
    }
    const T* operator->() const { return &value(); }
    const T& operator*() const { return value(); }

private:
    AnswerStatus status_ = AnswerStatus::not_asked;
    std::optional<T> value_;
};

// What a question function returns when no question is needed: the answer the handler then gets.
template <class T>
struct NoQuestion {
    Answer<T> answer;
};

// No question is needed, and the handler gets an answer that was not asked.
template <class T>
NoQuestion<T> no_question() {
    return NoQuestion<T>{};
}

// No question is needed, and the handler gets `accepted_value` as accepted.
template <class T>
NoQuestion<T> no_question(T accepted_value) {
    return NoQuestion<T>{Answer<T>(std::move(accepted_value))};
}

namespace detail {

// What asks for an input whose answers are T: the message of a form, a sampling request, a roots request.
template <class T>
using question_payload_t =
    std::conditional_t<std::is_same_v<T, SamplingAnswer>, SamplingRequest,
                       std::conditional_t<std::is_same_v<T, RootsAnswer>, RootsRequest, std::string>>;

}  // namespace detail

// What a question function returns: the question (a form's message, a sampling request, a roots request), or that
// no question is needed and what the handler gets instead (neomcp::no_question).
template <class T>
class Question {
public:
    using payload_type = detail::question_payload_t<T>;

    Question(payload_type asked_payload) : asked_(std::in_place_index<0>, std::move(asked_payload)) {}
    Question(const char* message)
        requires std::is_same_v<payload_type, std::string>
        : asked_(std::in_place_index<0>, std::string(message)) {}
    Question(NoQuestion<T> none) : asked_(std::in_place_index<1>, std::move(none.answer)) {}

    // Whether it asks; then payload() is the question, else answer() is what the handler gets.
    bool asks() const noexcept { return asked_.index() == 0; }
    const payload_type& payload() const { return std::get<0>(asked_); }
    const Answer<T>& answer() const { return std::get<1>(asked_); }

private:
    std::variant<payload_type, Answer<T>> asked_;
};

namespace detail {

// The kind of an input, from the type of its answers.
enum class InputKind { form, sampling, roots };

template <class T>
constexpr InputKind input_kind_of() {
    if constexpr (std::is_same_v<T, SamplingAnswer>) {
        return InputKind::sampling;
    } else if constexpr (std::is_same_v<T, RootsAnswer>) {
        return InputKind::roots;
    } else {
        return InputKind::form;
    }
}

// The question of an input declared without a question function: always asked, with the request's defaults.
struct AlwaysAsk {};

}  // namespace detail

// One declared input: its key, its question function, and whether it is optional. Made by neomcp::elicit,
// neomcp::sample and neomcp::roots.
template <class T, class Fn>
class InputDeclaration {
public:
    using answer_type = T;
    using question_type = Fn;

    InputDeclaration(std::string declared_key, Fn declared_question)
        : key_(std::move(declared_key)), question_(std::move(declared_question)) {}

    // The same input, optional (see the top of this file).
    InputDeclaration optional() const& {
        InputDeclaration copy(*this);
        copy.optional_ = true;
        return copy;
    }
    InputDeclaration optional() && {
        optional_ = true;
        return std::move(*this);
    }

    const std::string& input_key() const noexcept { return key_; }
    bool is_optional() const noexcept { return optional_; }
    const Fn& question_function() const noexcept { return question_; }

private:
    std::string key_;
    Fn question_;
    bool optional_ = false;
};

// A form the user fills in, whose answers decode as the described struct Form; `question` returns the message.
template <class Form, class Fn>
InputDeclaration<Form, std::decay_t<Fn>> elicit(std::string input_key, Fn&& question) {
    static_assert(described<Form>,
                  "neomcp::elicit<Form>: Form is a described struct (NEOMCP_DESCRIBE or a neomcp::Described "
                  "specialisation), whose members are the fields of the form");
    return InputDeclaration<Form, std::decay_t<Fn>>(std::move(input_key), std::forward<Fn>(question));
}

// A completion from the client's model; `question` returns the neomcp::SamplingRequest.
template <class Fn>
InputDeclaration<SamplingAnswer, std::decay_t<Fn>> sample(std::string input_key, Fn&& question) {
    return InputDeclaration<SamplingAnswer, std::decay_t<Fn>>(std::move(input_key), std::forward<Fn>(question));
}

// The client's roots, always asked.
inline InputDeclaration<RootsAnswer, detail::AlwaysAsk> roots(std::string input_key) {
    return InputDeclaration<RootsAnswer, detail::AlwaysAsk>(std::move(input_key), detail::AlwaysAsk{});
}

// The client's roots, asked when `question` returns neomcp::RootsRequest{}.
template <class Fn>
InputDeclaration<RootsAnswer, std::decay_t<Fn>> roots(std::string input_key, Fn&& question) {
    return InputDeclaration<RootsAnswer, std::decay_t<Fn>>(std::move(input_key), std::forward<Fn>(question));
}

namespace detail {

template <class D>
struct IsInputDeclaration : std::false_type {};
template <class T, class Fn>
struct IsInputDeclaration<InputDeclaration<T, Fn>> : std::true_type {};

// ---------------------------------------------------------------------------------------------------------------
// The rounds, as the library runs them

// One declared input as the library sees it.
struct DeclaredInput {
    std::string key;
    InputKind kind = InputKind::form;
    bool is_optional = false;
    json form_schema;                                  // a form: its requested schema
    std::optional<json_schema::Validator> form_check;  // a form: the validator of that schema
};

// Checks the declared inputs of `label` (a tool, a prompt, a resource or a template, as "tool 'x'") and compiles the
// checks of their forms with `limits`: every key is given and unique, every form fits the restricted form of one of
// the `revisions` served (empty: every built-in one; see form_schema_problem). Throws std::invalid_argument naming the
// thing, the input and what is wrong.
void prepare_inputs(std::span<DeclaredInput> inputs, const std::string& label, const json_schema::Limits& limits,
                    std::span<const profile::ProfilePtr> revisions);

// Thrown while a call with declared inputs is entered, for answers that cannot be used: an answer of another kind than
// its input's, a form's content that its schema refuses, outcomes too large to keep. The call is answered with Invalid
// params and the message.
class AnswerRefused : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// What one entry found for one input. Neither member: the input is asked in this round.
struct Settlement {
    std::optional<InputResponse> answer;  // the client's answer to exactly the input's current question
    bool not_asked = false;               // optional, and the client cannot take its request
};

// One entry of a call with declared inputs: the outcomes of the earlier rounds (from the state), the answers of the
// last round, and what this round asks.
class InputRound {
public:
    // Reads the state the call came with. A state that cannot be read counts as no earlier rounds, and the answers that
    // came with it answer nothing (they are asked again).
    InputRound(const Context& context, std::size_t max_state_bytes);

    // The outcome of `input` for `question`, its question in this entry: the one recorded for exactly this question,
    // or the one the client gave for it in this round; `not_asked` for an optional input whose request the client
    // cannot take; else nothing, and the input is asked in this round. Throws AnswerRefused for an answer that does not
    // fit its input.
    Settlement settle(const DeclaredInput& input, InputRequest question);

    // The requests of this round and the state that records the outcomes so far. Throws AnswerRefused when the state
    // would exceed the bound.
    InputRequired ask() const;

private:
    struct Recorded {
        std::string digest;  // of the question answered
        std::string method;  // of the request
        InputResponse answer;
    };

    const Context& context_;
    std::size_t max_state_bytes_;
    bool stateless_ = false;  // the call came with answers and without a state
    std::map<std::string, std::string> asked_before_;  // key -> digest of the question asked in the round before
    std::map<std::string, Recorded> recorded_;         // the outcomes the state recorded
    std::map<std::string, Recorded> kept_;             // the outcomes this round records
    std::map<std::string, InputRequest> requests_;     // asked in this round
    std::map<std::string, std::string> asking_;        // key -> digest of the question asked in this round
};

// ---------------------------------------------------------------------------------------------------------------
// The declarations of one handler

template <class... Decls>
struct InputSet {
    std::tuple<Decls...> declarations;
    std::array<DeclaredInput, sizeof...(Decls)> inputs;
    std::size_t max_state_bytes = 0;
};

template <class... Decls>
using AnswersOf = std::tuple<Answer<typename Decls::answer_type>...>;

// The first N elements of a tuple type.
template <std::size_t N, class Tuple, class Indexes = std::make_index_sequence<N>>
struct TuplePrefix;
template <std::size_t N, class... E, std::size_t... J>
struct TuplePrefix<N, std::tuple<E...>, std::index_sequence<J...>> {
    using type = std::tuple<std::tuple_element_t<J, std::tuple<E...>>...>;
};
template <std::size_t N, class Tuple>
using tuple_prefix_t = typename TuplePrefix<N, Tuple>::type;

template <class P, class A>
inline constexpr bool is_answer_param_v = std::is_same_v<P, A> || std::is_same_v<P, const A&>;

// Whether parameters Offset, Offset + 1, ... of Params take the answers of Answers, in order.
template <std::size_t Offset, class Params, class Answers, std::size_t... J>
constexpr bool answers_match(std::index_sequence<J...>) {
    return (true && ... && is_answer_param_v<param_t<Offset + J, Params>, std::tuple_element_t<J, Answers>>);
}

template <class Fn, class Params>
struct ConstInvocable : std::false_type {};
template <class Fn, class... P>
struct ConstInvocable<Fn, std::tuple<P...>> : std::bool_constant<std::is_invocable_v<const Fn&, P...>> {};

// What a question function takes.
enum class QuestionTakes { invalid, fixed, nothing, arguments, arguments_and_earlier, earlier };

template <class Args, class Fn, class Earlier>
constexpr QuestionTakes question_takes() {
    if constexpr (std::is_same_v<Fn, AlwaysAsk>) {
        return QuestionTakes::fixed;
    } else {
        using Sig = CallSignature<Fn>;
        if constexpr (!Sig::known) {
            return QuestionTakes::invalid;
        } else {
            using Params = typename Sig::params;
            constexpr bool with_args = !std::is_same_v<Args, NoArguments>;
            constexpr std::size_t earlier = std::tuple_size_v<Earlier>;
            if constexpr (Sig::arity == 0) {
                return QuestionTakes::nothing;
            } else if constexpr (with_args && is_arguments_param_v<param_t<0, Params>, Args> && Sig::arity == 1) {
                return QuestionTakes::arguments;
            } else if constexpr (with_args && earlier > 0 && Sig::arity == 1 + earlier &&
                                 is_arguments_param_v<param_t<0, Params>, Args> &&
                                 answers_match<1, Params, Earlier>(std::make_index_sequence<earlier>{})) {
                return QuestionTakes::arguments_and_earlier;
            } else if constexpr (!with_args && earlier > 0 && Sig::arity == earlier &&
                                 answers_match<0, Params, Earlier>(std::make_index_sequence<earlier>{})) {
                return QuestionTakes::earlier;
            } else {
                return QuestionTakes::invalid;
            }
        }
    }
}

template <class T, class R>
constexpr bool is_question_result() {
    using V = std::remove_cv_t<R>;
    return std::is_same_v<V, Question<T>> || std::is_same_v<V, NoQuestion<T>> ||
           std::is_convertible_v<V, question_payload_t<T>>;
}

// Checks the question function of input I at compile time.
template <class Args, std::size_t I, class Decl, class Answers>
constexpr bool check_question() {
    using T = typename Decl::answer_type;
    using Fn = typename Decl::question_type;
    using Sig = CallSignature<Fn>;
    constexpr QuestionTakes takes = question_takes<Args, Fn, tuple_prefix_t<I, Answers>>();
    if constexpr (takes == QuestionTakes::fixed) {
        return true;
    } else if constexpr (!Sig::known) {
        static_assert(dependent_false<Fn>,
                      "neomcp: a question function must be a function, a function pointer, or an object with exactly "
                      "one operator() that is not a template (write out the parameter types of a lambda instead of "
                      "auto)");
        return false;
    } else if constexpr (takes == QuestionTakes::invalid && !std::is_same_v<Args, NoArguments>) {
        static_assert(dependent_false<Fn>,
                      "neomcp: a question function takes nothing, the arguments (`const Args&` or `Args`), or the "
                      "arguments followed by the answers of every input declared before it, in order "
                      "(`const neomcp::Answer<T>&` or `neomcp::Answer<T>` each)");
        return false;
    } else if constexpr (takes == QuestionTakes::invalid) {
        static_assert(dependent_false<Fn>,
                      "neomcp: a question function of a handler without arguments takes nothing, or the answers of "
                      "every input declared before it, in order (`const neomcp::Answer<T>&` or `neomcp::Answer<T>` "
                      "each)");
        return false;
    } else if constexpr (!ConstInvocable<Fn, typename Sig::params>::value) {
        static_assert(dependent_false<Fn>,
                      "neomcp: a question function runs on several threads at once and must not change itself: it is "
                      "callable as const (a lambda that is not mutable)");
        return false;
    } else if constexpr (std::is_reference_v<typename Sig::result> ||
                         !is_question_result<T, typename Sig::result>()) {
        if constexpr (input_kind_of<T>() == InputKind::sampling) {
            static_assert(dependent_false<Fn>,
                          "neomcp: the question function of neomcp::sample returns a neomcp::SamplingRequest or a "
                          "neomcp::Question<neomcp::SamplingAnswer>, by value");
        } else if constexpr (input_kind_of<T>() == InputKind::roots) {
            static_assert(dependent_false<Fn>,
                          "neomcp: the question function of neomcp::roots returns neomcp::RootsRequest{} or a "
                          "neomcp::Question<neomcp::RootsAnswer>, by value");
        } else {
            static_assert(dependent_false<Fn>,
                          "neomcp: the question function of neomcp::elicit<Form> returns the message (std::string) or "
                          "a neomcp::Question<Form>, by value");
        }
        return false;
    } else {
        return true;
    }
}

template <class Args, class Answers, class... Decls, std::size_t... I>
constexpr bool check_questions(std::index_sequence<I...>) {
    return (true && ... && check_question<Args, I, std::tuple_element_t<I, std::tuple<Decls...>>, Answers>());
}

// Checks what follows a handler: declared inputs, each with a question function that fits.
template <class Args, class... Decls>
constexpr bool check_declarations() {
    if constexpr (!(true && ... && IsInputDeclaration<Decls>::value)) {
        static_assert(dependent_false<Args>,
                      "neomcp: what follows a handler are its declared inputs: neomcp::elicit<Form>(...), "
                      "neomcp::sample(...) or neomcp::roots(...)");
        return false;
    } else {
        return check_questions<Args, AnswersOf<Decls...>, Decls...>(std::index_sequence_for<Decls...>{});
    }
}

// What a handler with declared inputs is registered as; it decides which results it may return.
enum class DeclaredSite { tool, prompt, resource };

template <DeclaredSite site, class R>
constexpr bool site_accepts() {
    if constexpr (site == DeclaredSite::tool) {
        return is_plain_tool_result_type<R>();
    } else if constexpr (site == DeclaredSite::prompt) {
        return is_prompt_result_type<R>();
    } else {
        return is_resource_result_type<R>();
    }
}

// Checks a handler with declared inputs at compile time: the first problem found is reported.
template <DeclaredSite site, class Args, class F, class... Decls>
constexpr bool check_declared_handler() {
    using Sig = CallSignature<F>;
    if constexpr (!Sig::known) {
        static_assert(dependent_false<F>,
                      "neomcp: a handler must be a function, a function pointer, or an object with exactly one "
                      "operator() that is not a template (write out the parameter types of a lambda instead of auto)");
        return false;
    } else if constexpr (std::is_reference_v<typename Sig::result>) {
        static_assert(dependent_false<F>, "neomcp: a handler returns its result by value, not by reference");
        return false;
    } else {
        using Params = typename Sig::params;
        using Returned = std::remove_cv_t<typename Sig::result>;
        using R = std::remove_cv_t<typename AwaitableTraits<Returned>::value_type>;
        constexpr bool with_args = !std::is_same_v<Args, NoArguments>;
        constexpr std::size_t base = with_args ? 1 : 0;
        constexpr std::size_t count = sizeof...(Decls);
        constexpr std::size_t arity = Sig::arity;
        if constexpr (with_args && (arity == 0 || !is_arguments_param_v<param_t<0, Params>, Args>)) {
            static_assert(dependent_false<F>,
                          "neomcp: a handler with declared inputs takes the arguments first, as `const Args&` or "
                          "`Args`");
            return false;
        } else if constexpr (arity != base + count && arity != base + count + 1) {
            static_assert(dependent_false<F>,
                          "neomcp: a handler with declared inputs takes one `const neomcp::Answer<T>&` (or "
                          "`neomcp::Answer<T>`) per declared input, after the arguments and before an optional "
                          "`neomcp::Context&`; the number of its parameters does not match its inputs");
            return false;
        } else if constexpr (!answers_match<base, Params, AnswersOf<Decls...>>(std::make_index_sequence<count>{})) {
            static_assert(dependent_false<F>,
                          "neomcp: the answers a handler takes do not match its declared inputs: the n-th parameter "
                          "after the arguments is `const neomcp::Answer<T>&` (or `neomcp::Answer<T>`) for the input "
                          "declared n-th, where T is Form for neomcp::elicit<Form>, neomcp::SamplingAnswer for "
                          "neomcp::sample and neomcp::RootsAnswer for neomcp::roots");
            return false;
        } else if constexpr (arity == base + count + 1 && !is_context_param_v<param_t<base + count, Params>>) {
            static_assert(dependent_false<F>,
                          "neomcp: the parameter of a handler with declared inputs after its answers is "
                          "`neomcp::Context&`");
            return false;
        } else if constexpr (!AwaitableTraits<Returned>::default_executor) {
            static_assert(dependent_false<F>,
                          "neomcp: a coroutine handler returns asio::awaitable<R> with the default executor");
            return false;
        } else if constexpr (AskingResult<R>::asks) {
            static_assert(dependent_false<F>,
                          "neomcp: a handler with declared inputs returns its result and never neomcp::InputRequired: "
                          "its inputs are asked for it (a handler that asks by itself declares no inputs)");
            return false;
        } else if constexpr (is_c_string_v<R>) {
            static_assert(dependent_false<F>, "neomcp: a handler returns std::string, not a C string");
            return false;
        } else if constexpr (std::is_reference_v<R> || !site_accepts<site, R>()) {
            if constexpr (site == DeclaredSite::tool) {
                static_assert(dependent_false<F>,
                              "neomcp: a tool handler returns std::string, a described struct, bool, a number, "
                              "std::vector<T>, std::map<std::string, T>, a described enum, neomcp::json, "
                              "neomcp::ToolResult or void, or an asio::awaitable of one of them");
            } else if constexpr (site == DeclaredSite::prompt) {
                static_assert(dependent_false<F>,
                              "neomcp: a prompt handler returns std::string, neomcp::PromptMessage, "
                              "std::vector<neomcp::PromptMessage> or neomcp::PromptResult, or an asio::awaitable of "
                              "one of them");
            } else {
                static_assert(dependent_false<F>,
                              "neomcp: a resource handler returns std::string, neomcp::Bytes, a described struct, "
                              "neomcp::json, neomcp::ResourceContents, std::vector<neomcp::ResourceContents> or "
                              "neomcp::ReadResult, or an asio::awaitable of one of them");
            }
            return false;
        } else {
            return true;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Running the inputs of one entry

// A question function or a handler that always throws makes what follows its call unreachable in the instantiation
// for it, which one compiler reports as a warning in this header; the code is right for every other function.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4702)
#endif

template <class Decl>
DeclaredInput declared_input_of(const Decl& declaration) {
    using T = typename Decl::answer_type;
    DeclaredInput declared;
    declared.key = declaration.input_key();
    declared.kind = input_kind_of<T>();
    declared.is_optional = declaration.is_optional();
    if constexpr (input_kind_of<T>() == InputKind::form) declared.form_schema = form_schema_unchecked<T>();
    return declared;
}

template <class... Decls, std::size_t... I>
void describe_inputs(InputSet<Decls...>& set, std::index_sequence<I...>) {
    ((set.inputs[I] = declared_input_of(std::get<I>(set.declarations))), ...);
}

// The declarations of a handler, checked and prepared for `label`.
template <class... Decls>
InputSet<std::decay_t<Decls>...> make_input_set(const std::string& label, const json_schema::Limits& limits,
                                                std::span<const profile::ProfilePtr> revisions,
                                                std::size_t max_state_bytes, Decls&&... declarations) {
    InputSet<std::decay_t<Decls>...> set{std::tuple<std::decay_t<Decls>...>(std::forward<Decls>(declarations)...), {},
                                         max_state_bytes};
    describe_inputs(set, std::index_sequence_for<Decls...>{});
    prepare_inputs(std::span<DeclaredInput>(set.inputs), label, limits, revisions);
    return set;
}

// A copy of what a question function takes by value, or a reference to it.
template <class P, class V>
decltype(auto) lend(const V& held) {
    if constexpr (std::is_reference_v<P>) {
        return static_cast<const V&>(held);
    } else {
        return V(held);
    }
}

template <class T, class R>
Question<T> to_question(R&& produced) {
    using V = std::decay_t<R>;
    if constexpr (std::is_same_v<V, Question<T>> || std::is_same_v<V, NoQuestion<T>>) {
        return Question<T>(std::forward<R>(produced));
    } else {
        return Question<T>(question_payload_t<T>(std::forward<R>(produced)));
    }
}

template <class T, class Fn, class Args, class Answers, std::size_t... J>
Question<T> question_with_earlier(const Fn& question, const Args& args, const Answers& answers,
                                  std::index_sequence<J...>) {
    using Params = typename CallSignature<Fn>::params;
    if constexpr (std::is_same_v<Args, NoArguments>) {
        static_cast<void>(args);
        return to_question<T>(question(lend<param_t<J, Params>>(std::get<J>(answers))...));
    } else {
        return to_question<T>(
            question(lend<param_t<0, Params>>(args), lend<param_t<J + 1, Params>>(std::get<J>(answers))...));
    }
}

// The question of input I in this entry.
template <std::size_t I, class Args, class Decl, class Answers>
Question<typename Decl::answer_type> question_of(const Decl& declaration, const Args& args, const Answers& answers) {
    using T = typename Decl::answer_type;
    using Fn = typename Decl::question_type;
    constexpr QuestionTakes takes = question_takes<Args, Fn, tuple_prefix_t<I, Answers>>();
    if constexpr (takes == QuestionTakes::fixed) {
        static_cast<void>(declaration);
        static_cast<void>(args);
        static_cast<void>(answers);
        return Question<T>(question_payload_t<T>{});
    } else if constexpr (takes == QuestionTakes::nothing) {
        static_cast<void>(args);
        static_cast<void>(answers);
        return to_question<T>(declaration.question_function()());
    } else if constexpr (takes == QuestionTakes::arguments) {
        static_cast<void>(answers);
        using Params = typename CallSignature<Fn>::params;
        return to_question<T>(declaration.question_function()(lend<param_t<0, Params>>(args)));
    } else {
        return question_with_earlier<T>(declaration.question_function(), args, answers, std::make_index_sequence<I>{});
    }
}

template <class T>
InputRequest request_of(question_payload_t<T> payload, const DeclaredInput& declared) {
    if constexpr (input_kind_of<T>() == InputKind::form) {
        return ElicitationForm{std::move(payload), declared.form_schema};
    } else {
        return InputRequest(std::move(payload));
    }
}

// The answer a handler gets for what the client gave (InputRound checked its kind and a form's schema).
template <class T>
Answer<T> answer_from(const InputResponse& response, const DeclaredInput& declared) {
    if constexpr (input_kind_of<T>() == InputKind::form) {
        const auto* elicited = std::get_if<ElicitationAnswer>(&response);
        if (elicited == nullptr) throw AnswerRefused("the answer " + declared.key + " is not the answer of a form");
        if (elicited->action == ElicitationAction::decline) return Answer<T>(AnswerStatus::declined);
        if (elicited->action == ElicitationAction::cancel) return Answer<T>(AnswerStatus::cancelled);
        try {
            return Answer<T>(neomcp::from_json<T>(elicited->content));
        } catch (const DecodeError& error) {
            throw AnswerRefused("the answer " + declared.key + " does not fit its form: " + error.what());
        }
    } else {
        const auto* given = std::get_if<T>(&response);
        if (given == nullptr) throw AnswerRefused("the answer " + declared.key + " is not an answer of its request");
        return Answer<T>(*given);
    }
}

template <std::size_t I, class Args, class... Decls>
void settle_input(const InputSet<Decls...>& set, const Args& args, InputRound& entry_round, AnswersOf<Decls...>& answers,
                  std::array<bool, sizeof...(Decls)>& settled) {
    using Decl = std::tuple_element_t<I, std::tuple<Decls...>>;
    using T = typename Decl::answer_type;
    constexpr QuestionTakes takes = question_takes<Args, typename Decl::question_type, tuple_prefix_t<I, AnswersOf<Decls...>>>();
    if constexpr (takes == QuestionTakes::arguments_and_earlier || takes == QuestionTakes::earlier) {
        // A question that depends on earlier answers waits for all of them.
        for (std::size_t earlier = 0; earlier < I; ++earlier) {
            if (!settled[earlier]) return;
        }
    }
    Question<T> question = question_of<I>(std::get<I>(set.declarations), args, answers);
    if (!question.asks()) {
        std::get<I>(answers) = question.answer();
        settled[I] = true;
        return;
    }
    const DeclaredInput& declared = set.inputs[I];
    const Settlement found = entry_round.settle(declared, request_of<T>(question.payload(), declared));
    if (found.not_asked) {
        std::get<I>(answers) = Answer<T>();
        settled[I] = true;
    } else if (found.answer) {
        std::get<I>(answers) = answer_from<T>(*found.answer, declared);
        settled[I] = true;
    }
}

template <class Args, class... Decls, std::size_t... I>
void settle_inputs_at(const InputSet<Decls...>& set, const Args& args, InputRound& entry_round, AnswersOf<Decls...>& answers,
                      std::array<bool, sizeof...(Decls)>& settled, std::index_sequence<I...>) {
    (settle_input<I>(set, args, entry_round, answers, settled), ...);
}

// The inputs of one entry: empty when every input has its outcome (then in `answers`), else what this round asks.
template <class Args, class... Decls>
std::optional<InputRequired> settle_inputs(const InputSet<Decls...>& set, const Args& args, const Context& context,
                                           AnswersOf<Decls...>& answers) {
    InputRound entry_round(context, set.max_state_bytes);
    std::array<bool, sizeof...(Decls)> settled{};
    settle_inputs_at(set, args, entry_round, answers, settled, std::index_sequence_for<Decls...>{});
    for (const bool has_outcome : settled) {
        if (!has_outcome) return entry_round.ask();
    }
    return std::nullopt;
}

// An answer as the handler takes it: a reference, or moved into a by-value parameter.
template <class P, class A>
decltype(auto) pass_answer(A& held) {
    if constexpr (std::is_reference_v<P>) {
        return static_cast<const A&>(held);
    } else {
        return A(std::move(held));
    }
}

// Calls a handler with its arguments, its answers and, when it takes it, the context.
template <class Args, class F, class Answers, std::size_t... I>
std::remove_cv_t<typename CallSignature<F>::result> invoke_declared(F& handler, Args& args, Answers& answers,
                                                                    Context& context, std::index_sequence<I...>) {
    using Params = typename CallSignature<F>::params;
    constexpr bool with_args = !std::is_same_v<Args, NoArguments>;
    constexpr std::size_t base = with_args ? 1 : 0;
    constexpr bool takes_context = CallSignature<F>::arity == base + sizeof...(I) + 1;
    if constexpr (with_args && takes_context) {
        return handler(pass_arguments<param_t<0, Params>>(args),
                       pass_answer<param_t<base + I, Params>>(std::get<I>(answers))..., context);
    } else if constexpr (with_args) {
        static_cast<void>(context);
        return handler(pass_arguments<param_t<0, Params>>(args),
                       pass_answer<param_t<base + I, Params>>(std::get<I>(answers))...);
    } else if constexpr (takes_context) {
        static_cast<void>(args);
        return handler(pass_answer<param_t<base + I, Params>>(std::get<I>(answers))..., context);
    } else {
        static_cast<void>(args);
        static_cast<void>(context);
        return handler(pass_answer<param_t<base + I, Params>>(std::get<I>(answers))...);
    }
}

template <class Args, class F, class... Decls>
class DeclaredToolBody final : public PlainBody {
public:
    template <class H>
    DeclaredToolBody(H&& handler, InputSet<Decls...> set) : handler_(std::forward<H>(handler)), set_(std::move(set)) {}

    ToolOutcome run(const json& arguments, Context& context) override {
        using R = std::remove_cv_t<typename CallSignature<F>::result>;
        Args args = decode_arguments<Args>(arguments);
        AnswersOf<Decls...> answers;
        if (auto asked = settle_inputs(set_, args, context, answers)) {
            return ToolOutcome(std::in_place_index<1>, std::move(*asked));
        }
        if constexpr (std::is_void_v<R>) {
            invoke_declared(handler_, args, answers, context, std::index_sequence_for<Decls...>{});
            return ToolOutcome(std::in_place_index<0>, ToolResult{});
        } else {
            return to_tool_outcome<R>(
                invoke_declared(handler_, args, answers, context, std::index_sequence_for<Decls...>{}));
        }
    }

private:
    F handler_;
    InputSet<Decls...> set_;
};

// The outcome of a round that asks, for a coroutine body: built in a function that is never inlined. Built inside the
// coroutine, GCC 13 with optimization reports the destructor of the outcome's other alternative as reading memory
// that was never written (-Wmaybe-uninitialized), which it never does; that would fail the build of a user who turns
// warnings into errors.
#if defined(_MSC_VER)
#define NEOMCP_DETAIL_NOINLINE __declspec(noinline)
#else
#define NEOMCP_DETAIL_NOINLINE __attribute__((noinline))
#endif
template <class Outcome>
NEOMCP_DETAIL_NOINLINE Outcome asking_outcome(InputRequired asked) {
    return Outcome(std::in_place_index<1>, std::move(asked));
}
#undef NEOMCP_DETAIL_NOINLINE

template <class Args, class F, class... Decls>
class DeclaredToolCoroutineBody final : public CoroutineBody {
public:
    template <class H>
    DeclaredToolCoroutineBody(H&& handler, InputSet<Decls...> set)
        : handler_(std::forward<H>(handler)), set_(std::move(set)) {}

    asio::awaitable<ToolOutcome> run(const json& arguments, Context& context) override {
        using R = typename AwaitableTraits<std::remove_cv_t<typename CallSignature<F>::result>>::value_type;
        // The arguments and the answers live in this frame, which outlives the handler's coroutine.
        Args args = decode_arguments<Args>(arguments);
        AnswersOf<Decls...> answers;
        std::optional<InputRequired> asked = settle_inputs(set_, args, context, answers);
        if (asked) co_return asking_outcome<ToolOutcome>(std::move(*asked));
        auto started = invoke_declared(handler_, args, answers, context, std::index_sequence_for<Decls...>{});
        if constexpr (std::is_void_v<R>) {
            co_await std::move(started);
            co_return ToolOutcome(std::in_place_index<0>, ToolResult{});
        } else {
            R produced = co_await std::move(started);
            co_return to_tool_outcome<R>(std::move(produced));
        }
    }

private:
    F handler_;
    InputSet<Decls...> set_;
};

template <class Kind, class A, class F, class... Decls>
class DeclaredFeatureBody final : public FeaturePlainBody<typename Kind::Out> {
public:
    template <class H>
    DeclaredFeatureBody(H&& handler, InputSet<Decls...> set) : handler_(std::forward<H>(handler)), set_(std::move(set)) {}

    Produced<typename Kind::Out> run(const Invocation& invocation, Context& context) override {
        Inputs<A> inputs = Kind::template decode<A>(invocation);
        AnswersOf<Decls...> answers;
        if (auto asked = settle_inputs(set_, inputs.args, context, answers)) {
            return Produced<typename Kind::Out>(std::in_place_index<1>, std::move(*asked));
        }
        return produce<Kind>(invoke_declared(handler_, inputs.args, answers, context, std::index_sequence_for<Decls...>{}),
                             invocation);
    }

private:
    F handler_;
    InputSet<Decls...> set_;
};

template <class Kind, class A, class F, class... Decls>
class DeclaredFeatureCoroutineBody final : public FeatureCoroutineBody<typename Kind::Out> {
public:
    template <class H>
    DeclaredFeatureCoroutineBody(H&& handler, InputSet<Decls...> set)
        : handler_(std::forward<H>(handler)), set_(std::move(set)) {}

    asio::awaitable<Produced<typename Kind::Out>> run(const Invocation& invocation, Context& context) override {
        using R = typename AwaitableTraits<std::remove_cv_t<typename CallSignature<F>::result>>::value_type;
        // The inputs and the answers live in this frame, which outlives the handler's coroutine.
        Inputs<A> inputs = Kind::template decode<A>(invocation);
        AnswersOf<Decls...> answers;
        std::optional<InputRequired> asked = settle_inputs(set_, inputs.args, context, answers);
        if (asked) co_return asking_outcome<Produced<typename Kind::Out>>(std::move(*asked));
        auto started = invoke_declared(handler_, inputs.args, answers, context, std::index_sequence_for<Decls...>{});
        R produced = co_await std::move(started);
        co_return produce<Kind>(std::move(produced), invocation);
    }

private:
    F handler_;
    InputSet<Decls...> set_;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

template <class Args, class F, class H, class... Decls>
Body make_declared_tool_body(H&& handler, InputSet<Decls...> set) {
    if constexpr (is_coroutine_handler<F>()) {
        return Body(std::in_place_index<1>,
                    std::make_unique<DeclaredToolCoroutineBody<Args, F, Decls...>>(std::forward<H>(handler), std::move(set)));
    } else {
        return Body(std::in_place_index<0>,
                    std::make_unique<DeclaredToolBody<Args, F, Decls...>>(std::forward<H>(handler), std::move(set)));
    }
}

template <class Kind, class A, class F, class H, class... Decls>
FeatureBody<typename Kind::Out> make_declared_feature_body(H&& handler, InputSet<Decls...> set) {
    using Out = typename Kind::Out;
    if constexpr (is_coroutine_handler<F>()) {
        return FeatureBody<Out>(std::in_place_index<1>, std::make_unique<DeclaredFeatureCoroutineBody<Kind, A, F, Decls...>>(
                                                            std::forward<H>(handler), std::move(set)));
    } else {
        return FeatureBody<Out>(std::in_place_index<0>, std::make_unique<DeclaredFeatureBody<Kind, A, F, Decls...>>(
                                                            std::forward<H>(handler), std::move(set)));
    }
}

}  // namespace detail

}  // namespace neomcp
