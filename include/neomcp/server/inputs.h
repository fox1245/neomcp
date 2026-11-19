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
// An answer counts for the question that was asked: the call's state holds a digest of each question asked in the
// round before (the state is the handler's own; where the result asks it travels sealed, see neomcp/server.h).
//
// Everything below the public declarations is detail of neomcp/server.h.
#pragma once

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "neomcp/describe.h"
#include "neomcp/input.h"
#include "neomcp/json.h"
#include "neomcp/model.h"
#include "neomcp/server/form_schema.h"

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

}  // namespace detail

}  // namespace neomcp
