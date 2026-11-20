// How the library reads the form of a callable it is given (a server's tool handler, a client's input handler): its
// parameters and its result from its signature, and whether that result is a coroutine. Detail of neomcp/server.h and
// neomcp/client/input_handlers.h.
//
// A callable is a function pointer, or an object with exactly one operator() that is not a template (a lambda whose
// parameters are written out). Its result says whether it is a coroutine: asio::awaitable<R> is one that produces R.
#pragma once

#include <cstddef>
#include <tuple>
#include <type_traits>

#include <asio/awaitable.hpp>

namespace neomcp::detail {

template <class R, class... A>
struct Signature {
    static constexpr bool known = true;
    using result = R;
    using params = std::tuple<A...>;
    static constexpr std::size_t arity = sizeof...(A);
};

struct UnknownSignature {
    static constexpr bool known = false;
};

template <class M>
struct MemberSignature : UnknownSignature {};
template <class C, class R, class... A>
struct MemberSignature<R (C::*)(A...)> : Signature<R, A...> {};
template <class C, class R, class... A>
struct MemberSignature<R (C::*)(A...) const> : Signature<R, A...> {};
template <class C, class R, class... A>
struct MemberSignature<R (C::*)(A...) &> : Signature<R, A...> {};
template <class C, class R, class... A>
struct MemberSignature<R (C::*)(A...) const&> : Signature<R, A...> {};
template <class C, class R, class... A>
struct MemberSignature<R (C::*)(A...) noexcept> : Signature<R, A...> {};
template <class C, class R, class... A>
struct MemberSignature<R (C::*)(A...) const noexcept> : Signature<R, A...> {};
template <class C, class R, class... A>
struct MemberSignature<R (C::*)(A...) & noexcept> : Signature<R, A...> {};
template <class C, class R, class... A>
struct MemberSignature<R (C::*)(A...) const & noexcept> : Signature<R, A...> {};

// The signature of a callable of type F (decayed): a function pointer, or a class with one non-template operator().
template <class F, class = void>
struct CallSignature : UnknownSignature {};
template <class R, class... A>
struct CallSignature<R (*)(A...), void> : Signature<R, A...> {};
template <class R, class... A>
struct CallSignature<R (*)(A...) noexcept, void> : Signature<R, A...> {};
template <class F>
struct CallSignature<F, std::void_t<decltype(&F::operator())>> : MemberSignature<decltype(&F::operator())> {};

// The type of parameter N, or void when there are fewer parameters.
template <std::size_t N, class Params>
struct ParamAt {
    using type = void;
};
template <std::size_t N, class... A>
    requires(N < sizeof...(A))
struct ParamAt<N, std::tuple<A...>> {
    using type = std::tuple_element_t<N, std::tuple<A...>>;
};
template <std::size_t N, class Params>
using param_t = typename ParamAt<N, Params>::type;

// Whether a result is a coroutine (asio::awaitable), whether that coroutine has the default executor, and the value it
// produces (the result itself when it is no coroutine).
template <class T>
struct AwaitableTraits {
    static constexpr bool value = false;
    static constexpr bool default_executor = true;
    using value_type = T;
};
template <class T, class Executor>
struct AwaitableTraits<asio::awaitable<T, Executor>> {
    static constexpr bool value = true;
    static constexpr bool default_executor = std::is_same_v<Executor, asio::any_io_executor>;
    using value_type = T;
};

}  // namespace neomcp::detail
