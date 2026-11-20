// The client's answers to a server that asks for input while it serves a call: one handler per kind of request (a
// form, a URL to visit, a completion from the client's model, the client's roots), for every revision.
//
// A server asks in one of two ways, as the revision prescribes: it sends a request of its own while the call is
// pending, and the client answers it; or the call's result asks, and the client calls again with the answers. An
// application writes the same handler for both. The library declares a capability for each kind that has a handler and
// for no other kind, refuses what no handler takes, and runs the matching handler for the rest.
//
// A handler takes the request and, optionally, an InputContext&, and returns the answer, either as a plain function or
// as a coroutine:
//
//   form(h)          ElicitationAnswer(const ElicitationForm&, InputContext&)
//                    asio::awaitable<ElicitationAnswer>(ElicitationForm, InputContext&)
//   url(h)           ElicitationAnswer(const ElicitationUrl&, InputContext&)
//                    asio::awaitable<ElicitationAnswer>(ElicitationUrl, InputContext&)
//   sampling(h, s)   SamplingAnswer(const SamplingRequest&, InputContext&), or SamplingReply to let the user refuse
//                    asio::awaitable<SamplingAnswer>(SamplingRequest, InputContext&), or of SamplingReply
//   roots(h, n)      RootsAnswer(InputContext&)
//                    asio::awaitable<RootsAnswer>(InputContext&)
//
// The request may be taken as `const T&` or as `T` in either form, and the InputContext& may be left out. A coroutine
// returns asio::awaitable with the default executor. Anything else fails to compile with a static_assert that says
// what is expected. Registering a kind again replaces its handler.
//
// What the library does around a handler:
// - Forms: before a handler sees a form, its requested schema is checked against the restricted form the revision
//   defines and compiled within the peer limits; a form outside it never reaches the handler. The schema's defaults
//   are offered as ElicitationForm::prefilled(); the library adds nothing to an answer itself. Accepted content is
//   validated against the schema before it goes out, and content that fails is reported to the application and never
//   sent.
// - URLs: the URL is handed to the handler as data. The library never fetches it, opens it or looks inside it; showing
//   it and asking the user's consent before visiting it is the handler's.
// - A handler that throws: the server gets an internal error with a fixed message, never the exception's text; the
//   text is reported to the application only.
// - A sampling handler that returns InputRefused (through SamplingReply): the server is told that the user refused,
//   as the revision prescribes; under a revision whose result asks, the call ends without a retry.
// - When the server cancels its request, or the time the client may take lapses (input_handler_timeout), stop is
//   requested on the handler's InputContext and nothing it returns afterwards is sent.
//
// Threads. Plain handlers run on a worker pool of the client, bounded in size; coroutine handlers run on the client's
// own thread and must not block it. The handlers of one round of a call run concurrently, and so do the answers to
// several requests of a server, so one handler object may be called from several threads at once: it must be safe for
// that (a lambda that captures only what it reads is). A handler is copied into InputHandlers and shared by every copy
// of it.
#pragma once

#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include <asio/awaitable.hpp>

#include "neomcp/client/engine.h"
#include "neomcp/detail/callable.h"
#include "neomcp/input.h"

namespace neomcp {

namespace detail {
class InputDispatcherCore;
}  // namespace detail

// What a handler is told about the request it answers. It lives as long as the handler runs, and every member may be
// read from any thread.
class InputContext {
public:
    InputContext(const InputContext&) = delete;
    InputContext& operator=(const InputContext&) = delete;

    // Stop is requested when the server cancels its request, when the time the client may take to answer lapses, when
    // the call the request belongs to ends, and when the client closes. Nothing the handler returns afterwards is sent,
    // so it may give up at once.
    std::stop_token stop_token() const noexcept { return stop_; }
    bool cancelled() const noexcept { return stop_.stop_requested(); }

    // The method the server asked with, as the revision names it.
    const std::string& method() const noexcept { return method_; }
    // The revision the request was read under. Behaviour that differs between revisions is the library's job; this is
    // for logging and diagnostics.
    const std::string& revision() const noexcept { return revision_; }
    // The call the request belongs to: always when the call's result asked; when the server sent a request of its own,
    // only when the wire says which call it came with.
    const std::optional<RequestRef>& call() const noexcept { return call_; }
    // When the call's result asked: the key the server gave the request in its round.
    const std::optional<std::string>& key() const noexcept { return key_; }

private:
    friend class detail::InputDispatcherCore;
    InputContext(std::stop_token stop, std::string method, std::string revision, std::optional<RequestRef> call,
                 std::optional<std::string> key)
        : stop_(std::move(stop)),
          method_(std::move(method)),
          revision_(std::move(revision)),
          call_(call),
          key_(std::move(key)) {}

    std::stop_token stop_;
    std::string method_;
    std::string revision_;
    std::optional<RequestRef> call_;
    std::optional<std::string> key_;
};

// What a sampling handler can do besides answering: declare that it takes requests with tools, and requests that ask
// to include context from servers. A request that needs what is not declared is refused without reaching the handler.
struct SamplingSupport {
    bool tools = false;
    bool context = false;
};

// What a sampling handler returns when it lets the user refuse: the completion, or the refusal.
using SamplingReply = std::variant<SamplingAnswer, InputRefused>;

// ---------------------------------------------------------------------------------------------------------------
// Detail: the bodies InputHandlers keeps, with the handler's form erased.

namespace detail {

template <class T>
inline constexpr bool input_handler_unmatched = false;

// Runs on a worker thread; throws what the handler throws.
template <class Request, class Answer>
class PlainInputBody {
public:
    virtual ~PlainInputBody() = default;
    virtual Answer run(const Request& request, InputContext& context) = 0;
};

// Runs on the client's thread; the context outlives the coroutine.
template <class Request, class Answer>
class CoroutineInputBody {
public:
    virtual ~CoroutineInputBody() = default;
    virtual asio::awaitable<Answer> run(Request request, InputContext& context) = 0;
};

// One registered handler, in the form it was given; both empty when none is registered.
template <class Request, class Answer>
struct InputBody {
    std::shared_ptr<PlainInputBody<Request, Answer>> plain;
    std::shared_ptr<CoroutineInputBody<Request, Answer>> coroutine;
    explicit operator bool() const noexcept { return plain != nullptr || coroutine != nullptr; }
};

// The facts of an input handler of type F (decayed) for requests of type Request.
template <class Request, class F>
struct InputShape {
    using signature = CallSignature<F>;
    using returned = std::remove_cv_t<typename signature::result>;
    static constexpr bool coroutine = AwaitableTraits<returned>::value;
    using value = std::remove_cv_t<typename AwaitableTraits<returned>::value_type>;
    // A roots handler takes no request: there is nothing in one.
    static constexpr std::size_t inputs = std::is_same_v<Request, RootsRequest> ? 0 : 1;
    static constexpr bool takes_context = signature::arity == inputs + 1;
    using first = param_t<0, typename signature::params>;
};

template <class P, class Request>
inline constexpr bool is_request_param_v = std::is_same_v<P, Request> || std::is_same_v<P, const Request&>;

template <class P>
inline constexpr bool is_input_context_param_v = std::is_same_v<P, InputContext&>;

// Whether a handler of answers `Answer` may return `R`: the answer itself, or for sampling the reply that may refuse.
template <class Answer, class R>
inline constexpr bool is_input_value_v =
    std::is_same_v<R, Answer> || (std::is_same_v<Answer, SamplingReply> && std::is_same_v<R, SamplingAnswer>);

// Checks a handler at compile time: the first problem found is reported, and nothing after it is instantiated.
template <class Request, class Answer, class F>
constexpr bool check_input_handler() {
    using Sig = CallSignature<F>;
    if constexpr (!Sig::known) {
        static_assert(input_handler_unmatched<F>,
                      "neomcp: an input handler must be a function, a function pointer, or an object with exactly one "
                      "operator() that is not a template (write out the parameter types of a lambda instead of auto)");
        return false;
    } else if constexpr (std::is_reference_v<typename Sig::result>) {
        static_assert(input_handler_unmatched<F>, "neomcp: an input handler returns its answer by value");
        return false;
    } else {
        using S = InputShape<Request, F>;
        constexpr std::size_t arity = Sig::arity;
        using P0 = param_t<0, typename Sig::params>;
        using P1 = param_t<1, typename Sig::params>;
        if constexpr (S::inputs == 1 &&
                      (arity == 0 || arity > 2 || !is_request_param_v<P0, Request> ||
                       (arity == 2 && !is_input_context_param_v<P1>))) {
            static_assert(input_handler_unmatched<F>,
                          "neomcp: an input handler takes the request (as `const T&` or `T`) and optionally "
                          "`neomcp::InputContext&` after it");
            return false;
        } else if constexpr (S::inputs == 0 && (arity > 1 || (arity == 1 && !is_input_context_param_v<P0>))) {
            static_assert(input_handler_unmatched<F>,
                          "neomcp: a roots handler takes no parameters or only `neomcp::InputContext&`");
            return false;
        } else if constexpr (!AwaitableTraits<typename S::returned>::default_executor) {
            static_assert(input_handler_unmatched<F>,
                          "neomcp: a coroutine input handler returns asio::awaitable<T> with the default executor");
            return false;
        } else if constexpr (!is_input_value_v<Answer, typename S::value>) {
            static_assert(input_handler_unmatched<F>,
                          "neomcp: a form or URL handler returns neomcp::ElicitationAnswer, a sampling handler "
                          "neomcp::SamplingAnswer or neomcp::SamplingReply, a roots handler neomcp::RootsAnswer, or an "
                          "asio::awaitable of it");
            return false;
        } else {
            return true;
        }
    }
}

// Calls the handler with what its form takes.
template <class Request, class F, class Passed>
typename InputShape<Request, F>::returned invoke_input_handler(F& handler, Passed&& request, InputContext& context) {
    using S = InputShape<Request, F>;
    if constexpr (S::inputs == 0) {
        static_cast<void>(request);
        if constexpr (S::takes_context) {
            return handler(context);
        } else {
            static_cast<void>(context);
            return handler();
        }
    } else if constexpr (S::takes_context) {
        return handler(std::forward<Passed>(request), context);
    } else {
        static_cast<void>(context);
        return handler(std::forward<Passed>(request));
    }
}

// A handler that always throws makes the conversion of its answer unreachable in the instantiation for that handler,
// which one compiler reports as a warning in this header; the code is right for every other handler.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4702)
#endif

template <class Request, class Answer, class F>
class PlainInputBodyOf final : public PlainInputBody<Request, Answer> {
public:
    template <class H>
    explicit PlainInputBodyOf(H&& handler) : handler_(std::forward<H>(handler)) {}

    Answer run(const Request& request, InputContext& context) override {
        using P = typename InputShape<Request, F>::first;
        if constexpr (InputShape<Request, F>::inputs == 1 && !std::is_reference_v<P>) {
            return Answer(invoke_input_handler<Request>(handler_, Request(request), context));
        } else {
            return Answer(invoke_input_handler<Request>(handler_, request, context));
        }
    }

private:
    F handler_;
};

template <class Request, class Answer, class F>
class CoroutineInputBodyOf final : public CoroutineInputBody<Request, Answer> {
public:
    template <class H>
    explicit CoroutineInputBodyOf(H&& handler) : handler_(std::forward<H>(handler)) {}

    asio::awaitable<Answer> run(Request request, InputContext& context) override {
        using P = typename InputShape<Request, F>::first;
        // The request lives in this frame, which outlives the handler's coroutine: a reference parameter is safe.
        if constexpr (InputShape<Request, F>::inputs == 1 && !std::is_reference_v<P>) {
            auto value = co_await invoke_input_handler<Request>(handler_, std::move(request), context);
            co_return Answer(std::move(value));
        } else {
            auto value = co_await invoke_input_handler<Request>(handler_, request, context);
            co_return Answer(std::move(value));
        }
    }

private:
    F handler_;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

template <class Request, class Answer, class H>
InputBody<Request, Answer> make_input_body(H&& handler) {
    using F = std::decay_t<H>;
    InputBody<Request, Answer> body;
    if constexpr (check_input_handler<Request, Answer, F>()) {
        if constexpr (InputShape<Request, F>::coroutine) {
            body.coroutine = std::make_shared<CoroutineInputBodyOf<Request, Answer, F>>(std::forward<H>(handler));
        } else {
            body.plain = std::make_shared<PlainInputBodyOf<Request, Answer, F>>(std::forward<H>(handler));
        }
    }
    return body;
}

}  // namespace detail

// ---------------------------------------------------------------------------------------------------------------
// The handlers

// One handler per kind of input request; see the top of this file for their forms. Cheap to copy: copies share the
// handlers.
class InputHandlers {
public:
    // Elicitation in form mode.
    template <class H>
    InputHandlers& form(H&& handler) {
        form_ = detail::make_input_body<ElicitationForm, ElicitationAnswer>(std::forward<H>(handler));
        return *this;
    }
    // Elicitation in URL mode, where the revision has it.
    template <class H>
    InputHandlers& url(H&& handler) {
        url_ = detail::make_input_body<ElicitationUrl, ElicitationAnswer>(std::forward<H>(handler));
        return *this;
    }
    // Sampling, with what the handler takes besides plain requests.
    template <class H>
    InputHandlers& sampling(H&& handler, SamplingSupport support = {}) {
        sampling_ = detail::make_input_body<SamplingRequest, SamplingReply>(std::forward<H>(handler));
        sampling_support_ = support;
        return *this;
    }
    // The client's roots; `notify_changes` declares that the client tells the server when they change
    // (Client::roots_changed), where the revision has a notification for that.
    template <class H>
    InputHandlers& roots(H&& handler, bool notify_changes = false) {
        roots_ = detail::make_input_body<RootsRequest, RootsAnswer>(std::forward<H>(handler));
        roots_notify_ = notify_changes;
        return *this;
    }

    // The kinds that have a handler: what the client declares (EngineOptions::input).
    InputSupport support() const noexcept;
    // Whether no handler is registered.
    bool empty() const noexcept;

private:
    friend class detail::InputDispatcherCore;

    detail::InputBody<ElicitationForm, ElicitationAnswer> form_;
    detail::InputBody<ElicitationUrl, ElicitationAnswer> url_;
    detail::InputBody<SamplingRequest, SamplingReply> sampling_;
    detail::InputBody<RootsRequest, RootsAnswer> roots_;
    SamplingSupport sampling_support_;
    bool roots_notify_ = false;
};

}  // namespace neomcp
