// LoopbackRedirect against browsers that are plain HTTP requests: the redirect URI and the port, the one request the
// prompt waits for, what does not end the wait (another path, another method, a request over the bound), what the
// browser is shown, the closing of the port at the end and on cancellation, the address the listener is bound to, and
// the prompts that cannot start. The prompt is driven directly, as the authorizer drives it.
#include <gtest/gtest.h>

#include <asio/co_spawn.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/use_future.hpp>

#include <atomic>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>

#include "loopback_support.h"
#include "neomcp/http/listener.h"
#include "neomcp/http/message.h"

namespace {

using namespace loopback_test;
using neomcp::LoopbackRedirect;
using neomcp::LoopbackRedirectOptions;

constexpr std::string_view callback_query = "code=abc%20d&state=xyz&note=%2F%3F";

// ---- The redirect URI

TEST(LoopbackRedirectCreate, GivesARedirectUriOnAPortTheSystemChoseAndLeavesItClosed) {
    Opened opened;
    const auto redirect = make_redirect(opened);
    ASSERT_TRUE(redirect);
    EXPECT_NE(redirect->port(), 0);
    EXPECT_EQ(redirect->redirect_uri(), "http://127.0.0.1:" + std::to_string(redirect->port()) + "/callback");
    // The port is open for a sign-in only.
    EXPECT_FALSE(connects(redirect->port()));
    EXPECT_TRUE(opened.urls().empty());
}

TEST(LoopbackRedirectCreate, UsesThePortAndThePathItWasGiven) {
    std::uint16_t free_port = 0;
    {
        Opened probe_opened;
        const auto probe = make_redirect(probe_opened);
        ASSERT_TRUE(probe);
        free_port = probe->port();
    }
    Opened opened;
    LoopbackRedirectOptions options;
    options.port = free_port;
    options.path = "/oauth/return-1";
    const auto redirect = make_redirect(opened, std::move(options));
    ASSERT_TRUE(redirect);
    EXPECT_EQ(redirect->port(), free_port);
    EXPECT_EQ(redirect->redirect_uri(), "http://127.0.0.1:" + std::to_string(free_port) + "/oauth/return-1");

    RunningPrompt prompt(redirect->plain_prompt(), prompt_info(*redirect));
    ASSERT_TRUE(opened.wait(1));
    EXPECT_EQ(get(free_port, "/oauth/return-1?code=1").status, 200);
    const auto answer = prompt.answer();
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer->callback_url, std::optional<std::string>(redirect->redirect_uri() + "?code=1"));
}

TEST(LoopbackRedirectCreate, RefusesOptionsThatCannotWork) {
    const auto expect_invalid = [](LoopbackRedirectOptions options, const char* what) {
        const auto made = LoopbackRedirect::create(std::move(options));
        ASSERT_FALSE(made) << what;
        EXPECT_EQ(made.error().kind, neomcp::ClientErrorKind::invalid_options) << what;
        EXPECT_FALSE(made.error().message.empty()) << what;
    };
    LoopbackRedirectOptions without_browser;
    expect_invalid(without_browser, "no function that opens the browser");

    Opened opened;
    const auto with_browser = [&] {
        LoopbackRedirectOptions options;
        options.open_browser = opened.function();
        return options;
    };
    auto bad_path = with_browser();
    bad_path.path = "callback";
    expect_invalid(bad_path, "a path without a leading slash");
    auto dotted = with_browser();
    dotted.path = "/a/../b";
    expect_invalid(dotted, "a path with a dot segment");
    auto no_bound = with_browser();
    no_bound.max_callback_bytes = 0;
    expect_invalid(no_bound, "a bound of zero");
    auto no_time = with_browser();
    no_time.read_timeout = std::chrono::milliseconds(0);
    expect_invalid(no_time, "a read timeout of zero");

    // A port that is in use.
    neomcp::http::ListenerOptions taken;
    neomcp::http::Listener holder(taken, [](neomcp::http::Request, neomcp::http::Responder responder) {
        responder.complete(neomcp::http::make_response(204));
    });
    const auto bound = holder.start();
    ASSERT_TRUE(bound) << bound.error;
    auto in_use = with_browser();
    in_use.port = *bound.value;
    expect_invalid(in_use, "a port in use");
}

// ---- The one request

TEST(LoopbackRedirectPrompt, ReturnsTheCallbackUrlWholeAndShowsTheBrowserAFixedPage) {
    Opened opened;
    const auto redirect = make_redirect(opened);
    ASSERT_TRUE(redirect);
    RunningPrompt prompt(redirect->plain_prompt(), prompt_info(*redirect, "https://as.example/authorize?s=1&t=%20"));
    ASSERT_TRUE(opened.wait(1));
    // The authorization URL reaches the application as it is.
    EXPECT_EQ(opened.urls().front(), "https://as.example/authorize?s=1&t=%20");

    const Reply shown = get(redirect->port(), "/callback?" + std::string(callback_query));
    EXPECT_EQ(shown.status, 200) << shown.error;
    EXPECT_FALSE(shown.body.empty());
    // Whatever came back is not shown: the page does not depend on the query.
    EXPECT_EQ(shown.body.find("abc"), std::string::npos);
    EXPECT_EQ(shown.body.find("xyz"), std::string::npos);
    const Reply plain = shown;

    const auto answer = prompt.answer();
    ASSERT_TRUE(answer);
    // Every character of the query as the browser sent it, and the redirect URI's scheme, host, port and path.
    EXPECT_EQ(answer->callback_url,
              std::optional<std::string>(redirect->redirect_uri() + "?" + std::string(callback_query)));

    // The same page for a sign-in that was refused.
    RunningPrompt second(redirect->plain_prompt(), prompt_info(*redirect));
    ASSERT_TRUE(opened.wait(2));
    const Reply refused = get(redirect->port(), "/callback?error=access_denied&state=xyz");
    EXPECT_EQ(refused.status, 200);
    EXPECT_EQ(refused.body, plain.body);
    ASSERT_TRUE(second.answer());
}

TEST(LoopbackRedirectPrompt, AnswersARequestWithoutAQueryToo) {
    Opened opened;
    const auto redirect = make_redirect(opened);
    ASSERT_TRUE(redirect);
    RunningPrompt prompt(redirect->plain_prompt(), prompt_info(*redirect));
    ASSERT_TRUE(opened.wait(1));
    EXPECT_EQ(get(redirect->port(), "/callback").status, 200);
    const auto answer = prompt.answer();
    ASSERT_TRUE(answer);
    // The authorizer rejects it (no state); the listener does not read the query.
    EXPECT_EQ(answer->callback_url, std::optional<std::string>(redirect->redirect_uri()));
}

TEST(LoopbackRedirectPrompt, ListensBeforeTheBrowserIsOpened) {
    Opened opened;
    std::atomic<bool> listening{false};
    const auto redirect = make_redirect(opened);
    ASSERT_TRUE(redirect);
    const std::uint16_t port = redirect->port();
    opened.then([&] { listening = connects(port); });
    RunningPrompt prompt(redirect->plain_prompt(), prompt_info(*redirect));
    ASSERT_TRUE(opened.wait(1));
    // The browser can return at once; the port was open when it was told to start.
    EXPECT_TRUE(get(port, "/callback?code=1").status == 200);
    ASSERT_TRUE(prompt.answer());
    EXPECT_TRUE(listening.load());
}

TEST(LoopbackRedirectPrompt, ClosesThePortOnceTheBrowserCameBackAndTakesNoSecondRequest) {
    Opened opened;
    const auto redirect = make_redirect(opened);
    ASSERT_TRUE(redirect);
    RunningPrompt prompt(redirect->plain_prompt(), prompt_info(*redirect));
    ASSERT_TRUE(opened.wait(1));
    EXPECT_TRUE(connects(redirect->port()));
    EXPECT_EQ(get(redirect->port(), "/callback?code=first&state=s").status, 200);
    const auto answer = prompt.answer();
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer->callback_url, std::optional<std::string>(redirect->redirect_uri() + "?code=first&state=s"));
    // The prompt returns after the port is closed.
    EXPECT_FALSE(connects(redirect->port()));
    EXPECT_NE(get(redirect->port(), "/callback?code=second&state=s").status, 200);
    EXPECT_TRUE(redirect->last_problem().empty());
}

// The prompt closes the listener as soon as the request is taken; the page must still reach the browser (a listener that
// is stopped before the answer is written answers 503 instead).
TEST(LoopbackRedirectPrompt, TheBrowserGetsThePageWhateverTheTimingOfTheClose) {
    Opened opened;
    const auto redirect = make_redirect(opened);
    ASSERT_TRUE(redirect);
    for (int round = 1; round <= 150; ++round) {
        RunningPrompt prompt(redirect->plain_prompt(), prompt_info(*redirect));
        ASSERT_TRUE(opened.wait(static_cast<std::size_t>(round)));
        const Reply shown = get(redirect->port(), "/callback?code=" + std::to_string(round));
        ASSERT_EQ(shown.status, 200) << "round " << round << ": " << shown.error;
        ASSERT_TRUE(prompt.answer()) << "round " << round;
    }
}

// Two browsers come back at once: one request is taken, the other is not answered with the page, and the answer is the
// URL of the one that was.
TEST(LoopbackRedirectPrompt, OnlyOneOfTwoRequestsAtOnceIsTaken) {
    Opened opened;
    const auto redirect = make_redirect(opened);
    ASSERT_TRUE(redirect);
    for (int round = 1; round <= 60; ++round) {
        RunningPrompt prompt(redirect->plain_prompt(), prompt_info(*redirect));
        ASSERT_TRUE(opened.wait(static_cast<std::size_t>(round)));
        std::promise<void> go;
        const std::shared_future<void> ready = go.get_future().share();
        std::vector<Reply> replies(2);
        std::vector<std::thread> browsers;
        for (std::size_t one = 0; one < replies.size(); ++one) {
            browsers.emplace_back([&, one] {
                ready.wait();
                replies[one] = get(redirect->port(), "/callback?code=" + std::to_string(one));
            });
        }
        go.set_value();
        for (std::thread& browser : browsers) browser.join();
        const auto answer = prompt.answer();
        ASSERT_TRUE(answer) << "round " << round;
        ASSERT_TRUE(answer->callback_url) << "round " << round;
        std::size_t pages = 0;
        for (std::size_t one = 0; one < replies.size(); ++one) {
            if (replies[one].status != 200) continue;
            ++pages;
            EXPECT_EQ(*answer->callback_url, redirect->redirect_uri() + "?code=" + std::to_string(one))
                << "round " << round;
        }
        ASSERT_EQ(pages, 1u) << "round " << round;
    }
}

TEST(LoopbackRedirectPrompt, ServesTheNextSignInOnTheSamePort) {
    Opened opened;
    const auto redirect = make_redirect(opened);
    ASSERT_TRUE(redirect);
    for (int round = 1; round <= 3; ++round) {
        RunningPrompt prompt(redirect->plain_prompt(), prompt_info(*redirect));
        ASSERT_TRUE(opened.wait(static_cast<std::size_t>(round)));
        EXPECT_EQ(get(redirect->port(), "/callback?code=" + std::to_string(round)).status, 200);
        const auto answer = prompt.answer();
        ASSERT_TRUE(answer);
        EXPECT_EQ(answer->callback_url,
                  std::optional<std::string>(redirect->redirect_uri() + "?code=" + std::to_string(round)));
        EXPECT_FALSE(connects(redirect->port()));
    }
}

// ---- What does not end the wait

TEST(LoopbackRedirectPrompt, ARequestToAnotherPathDoesNotEndTheWait) {
    Opened opened;
    const auto redirect = make_redirect(opened);
    ASSERT_TRUE(redirect);
    RunningPrompt prompt(redirect->plain_prompt(), prompt_info(*redirect));
    ASSERT_TRUE(opened.wait(1));
    for (const char* other : {"/", "/favicon.ico", "/callback/", "/callbacks?code=x", "/other?code=x&state=s",
                              "/callback/extra?code=x"}) {
        EXPECT_EQ(get(redirect->port(), other).status, 404) << other;
    }
    EXPECT_TRUE(prompt.still_waiting());
    EXPECT_TRUE(connects(redirect->port()));
    EXPECT_EQ(get(redirect->port(), "/callback?code=real&state=s").status, 200);
    const auto answer = prompt.answer();
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer->callback_url, std::optional<std::string>(redirect->redirect_uri() + "?code=real&state=s"));
}

TEST(LoopbackRedirectPrompt, ARequestOfAnotherMethodIsRefusedAndTheWaitGoesOn) {
    Opened opened;
    const auto redirect = make_redirect(opened);
    ASSERT_TRUE(redirect);
    RunningPrompt prompt(redirect->plain_prompt(), prompt_info(*redirect));
    ASSERT_TRUE(opened.wait(1));
    for (const char* method : {"POST", "PUT", "DELETE", "HEAD"}) {
        const Reply refused = request(redirect->port(), method, "/callback?code=evil&state=s");
        EXPECT_EQ(refused.status, 405) << method << " " << refused.error;
        EXPECT_EQ(refused.allow, "GET") << method;
    }
    EXPECT_TRUE(prompt.still_waiting());
    EXPECT_EQ(get(redirect->port(), "/callback?code=good&state=s").status, 200);
    const auto answer = prompt.answer();
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer->callback_url, std::optional<std::string>(redirect->redirect_uri() + "?code=good&state=s"));
}

TEST(LoopbackRedirectPrompt, ARequestOverTheBoundIsRefusedAndTheWaitGoesOn) {
    Opened opened;
    LoopbackRedirectOptions options;
    options.max_callback_bytes = 200;
    const auto redirect = make_redirect(opened, std::move(options));
    ASSERT_TRUE(redirect);
    RunningPrompt prompt(redirect->plain_prompt(), prompt_info(*redirect));
    ASSERT_TRUE(opened.wait(1));

    // The callback URL is counted whole, scheme and host included: the longest query that fits is the bound less
    // the redirect URI and the '?'.
    const std::size_t room = 200 - redirect->redirect_uri().size() - 1;
    const Reply over = get(redirect->port(), "/callback?code=" + std::string(room, 'a'));
    EXPECT_EQ(over.status, 414) << over.error;
    EXPECT_TRUE(prompt.still_waiting());

    const std::string exact = "code=" + std::string(room - 5, 'b');
    EXPECT_EQ(get(redirect->port(), "/callback?" + exact).status, 200);
    const auto answer = prompt.answer();
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer->callback_url, std::optional<std::string>(redirect->redirect_uri() + "?" + exact));
    EXPECT_EQ(answer->callback_url->size(), std::size_t{200});
}

// The default bound is far over what the listener reads of a request line: a very long one is refused by the listener
// and does not end the wait either.
TEST(LoopbackRedirectPrompt, AVeryLongRequestDoesNotEndTheWait) {
    Opened opened;
    const auto redirect = make_redirect(opened);
    ASSERT_TRUE(redirect);
    RunningPrompt prompt(redirect->plain_prompt(), prompt_info(*redirect));
    ASSERT_TRUE(opened.wait(1));
    const Reply over = get(redirect->port(), "/callback?code=" + std::string(64 * 1024, 'a'));
    EXPECT_NE(over.status, 200) << over.error;
    EXPECT_TRUE(prompt.still_waiting());
    EXPECT_EQ(get(redirect->port(), "/callback?code=ok").status, 200);
    const auto answer = prompt.answer();
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer->callback_url, std::optional<std::string>(redirect->redirect_uri() + "?code=ok"));
}

TEST(LoopbackRedirectPrompt, ConnectionsThatSayNothingDoNotKeepTheBrowserOut) {
    Opened opened;
    LoopbackRedirectOptions options;
    options.read_timeout = std::chrono::milliseconds(2000);
    const auto redirect = make_redirect(opened, std::move(options));
    ASSERT_TRUE(redirect);
    RunningPrompt prompt(redirect->plain_prompt(), prompt_info(*redirect));
    ASSERT_TRUE(opened.wait(1));
    // Connections that say nothing, as a browser's spare ones do.
    asio::io_context io;
    std::vector<asio::ip::tcp::socket> idle;
    for (int i = 0; i < 3; ++i) {
        idle.emplace_back(io);
        std::error_code error;
        idle.back().connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), redirect->port()), error);
        ASSERT_FALSE(error);
    }
    EXPECT_EQ(get(redirect->port(), "/callback?code=1").status, 200);
    ASSERT_TRUE(prompt.answer());
    EXPECT_FALSE(connects(redirect->port()));
}

// ---- Cancellation

TEST(LoopbackRedirectPrompt, CancellationEndsThePlainPromptWithoutAnAnswerAndClosesThePort) {
    Opened opened;
    const auto redirect = make_redirect(opened);
    ASSERT_TRUE(redirect);
    RunningPrompt prompt(redirect->plain_prompt(), prompt_info(*redirect));
    ASSERT_TRUE(opened.wait(1));
    EXPECT_TRUE(connects(redirect->port()));
    EXPECT_TRUE(prompt.still_waiting());
    prompt.stop();
    const auto answer = prompt.answer();
    ASSERT_TRUE(answer);
    EXPECT_FALSE(answer->callback_url);
    EXPECT_FALSE(connects(redirect->port()));
    EXPECT_NE(get(redirect->port(), "/callback?code=late&state=s").status, 200);
}

TEST(LoopbackRedirectPrompt, ACancelledPromptDoesNotOpenTheBrowser) {
    Opened opened;
    const auto redirect = make_redirect(opened);
    ASSERT_TRUE(redirect);
    std::stop_source stop;
    stop.request_stop();
    neomcp::AuthorizationPromptContext context(stop.get_token());
    const auto prompt = redirect->plain_prompt();
    const neomcp::AuthorizationPromptAnswer answer = prompt.plain()->run(prompt_info(*redirect), context);
    EXPECT_FALSE(answer.callback_url);
    EXPECT_TRUE(opened.urls().empty());
    EXPECT_FALSE(connects(redirect->port()));
}

// ---- The coroutine form

// The client's thread, as a thread of its own that runs an io_context.
class ClientThread {
public:
    ClientThread() : work_(asio::make_work_guard(context_)), thread_([this] { context_.run(); }) {}
    ~ClientThread() {
        work_.reset();
        context_.stop();
        if (thread_.joinable()) thread_.join();
    }
    asio::io_context& context() { return context_; }
    std::thread::id id() const { return thread_.get_id(); }

private:
    asio::io_context context_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::thread thread_;
};

// A coroutine prompt running on the client thread.
class RunningCoroutine {
public:
    RunningCoroutine(ClientThread& client, neomcp::AuthorizationPrompt prompt, neomcp::AuthorizationPromptInfo info)
        : context_(stop_.get_token()), body_(prompt.coroutine()) {
        result_ = asio::co_spawn(client.context(), body_->run(std::move(info), context_), asio::use_future);
    }
    ~RunningCoroutine() {
        stop_.request_stop();
        if (result_.valid()) result_.wait();
    }
    void stop() { stop_.request_stop(); }
    std::optional<neomcp::AuthorizationPromptAnswer> answer() {
        if (result_.wait_for(patience) != std::future_status::ready) return std::nullopt;
        return result_.get();
    }
    bool still_waiting() const { return result_.wait_for(window) != std::future_status::ready; }

private:
    std::stop_source stop_;
    neomcp::AuthorizationPromptContext context_;
    std::shared_ptr<neomcp::detail::CoroutinePromptBody> body_;
    std::future<neomcp::AuthorizationPromptAnswer> result_;
};

TEST(LoopbackRedirectCoroutine, ReturnsTheCallbackUrlAndClosesThePortWithoutBlockingTheClientsThread) {
    ClientThread client;
    Opened opened;
    const auto redirect = make_redirect(opened);
    ASSERT_TRUE(redirect);
    RunningCoroutine prompt(client, redirect->coroutine_prompt(), prompt_info(*redirect));
    ASSERT_TRUE(opened.wait(1));
    ASSERT_EQ(opened.threads().size(), 1u);
    EXPECT_EQ(opened.threads().front(), client.id());

    // While the prompt waits the client's thread runs other work.
    std::promise<void> ran;
    asio::post(client.context(), [&] { ran.set_value(); });
    EXPECT_EQ(ran.get_future().wait_for(patience), std::future_status::ready);

    EXPECT_EQ(get(redirect->port(), "/other").status, 404);
    EXPECT_TRUE(prompt.still_waiting());
    EXPECT_EQ(get(redirect->port(), "/callback?" + std::string(callback_query)).status, 200);
    const auto answer = prompt.answer();
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer->callback_url,
              std::optional<std::string>(redirect->redirect_uri() + "?" + std::string(callback_query)));
    EXPECT_FALSE(connects(redirect->port()));
}

TEST(LoopbackRedirectCoroutine, CancellationEndsThePromptWithoutAnAnswerAndClosesThePort) {
    ClientThread client;
    Opened opened;
    const auto redirect = make_redirect(opened);
    ASSERT_TRUE(redirect);
    RunningCoroutine prompt(client, redirect->coroutine_prompt(), prompt_info(*redirect));
    ASSERT_TRUE(opened.wait(1));
    EXPECT_TRUE(connects(redirect->port()));
    prompt.stop();
    const auto answer = prompt.answer();
    ASSERT_TRUE(answer);
    EXPECT_FALSE(answer->callback_url);
    EXPECT_FALSE(connects(redirect->port()));
}

TEST(LoopbackRedirectCoroutine, ASignInAfterACancelledOneWaitsAgain) {
    ClientThread client;
    Opened opened;
    const auto redirect = make_redirect(opened);
    ASSERT_TRUE(redirect);
    {
        RunningCoroutine first(client, redirect->coroutine_prompt(), prompt_info(*redirect));
        ASSERT_TRUE(opened.wait(1));
        first.stop();
        ASSERT_TRUE(first.answer());
    }
    RunningCoroutine second(client, redirect->coroutine_prompt(), prompt_info(*redirect));
    ASSERT_TRUE(opened.wait(2));
    EXPECT_EQ(get(redirect->port(), "/callback?code=again").status, 200);
    const auto answer = second.answer();
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer->callback_url, std::optional<std::string>(redirect->redirect_uri() + "?code=again"));
}

// ---- The address

TEST(LoopbackRedirectPrompt, TheListenerIsBoundToTheLoopbackAddressOnly) {
    Opened opened;
    const auto redirect = make_redirect(opened);
    ASSERT_TRUE(redirect);
    RunningPrompt prompt(redirect->plain_prompt(), prompt_info(*redirect));
    ASSERT_TRUE(opened.wait(1));
    const std::uint16_t port = redirect->port();
    // Something listens, so that the refusals below are not those of a port nobody opened.
    ASSERT_TRUE(connects(port));
    // Not on another address of the loopback range, and not on the IPv6 loopback.
    EXPECT_FALSE(connects(asio::ip::make_address("127.0.0.2"), port));
    EXPECT_FALSE(connects(asio::ip::make_address("::1"), port));
    // Not on the address this machine reaches other networks from.
    if (const auto outbound = outbound_address()) {
        EXPECT_FALSE(connects(*outbound, port)) << outbound->to_string();
    } else {
        GTEST_LOG_(INFO) << "this machine has no address but the loopback ones; only those were checked";
    }
    EXPECT_TRUE(connects(port));
}

// ---- Prompts that cannot start

TEST(LoopbackRedirectPrompt, APortThatWasTakenInBetweenEndsThePromptAndSaysWhy) {
    Opened opened;
    const auto redirect = make_redirect(opened);
    ASSERT_TRUE(redirect);
    neomcp::http::ListenerOptions taken;
    taken.port = redirect->port();
    neomcp::http::Listener holder(taken, [](neomcp::http::Request, neomcp::http::Responder responder) {
        responder.complete(neomcp::http::make_response(204));
    });
    const auto bound = holder.start();
    ASSERT_TRUE(bound) << bound.error;

    RunningPrompt prompt(redirect->plain_prompt(), prompt_info(*redirect));
    const auto answer = prompt.answer();
    ASSERT_TRUE(answer);
    EXPECT_FALSE(answer->callback_url);
    EXPECT_FALSE(redirect->last_problem().empty());
    EXPECT_TRUE(opened.urls().empty());

    // Once the port is free again the next sign-in works.
    holder.stop();
    RunningPrompt again(redirect->plain_prompt(), prompt_info(*redirect));
    ASSERT_TRUE(opened.wait(1));
    EXPECT_EQ(get(redirect->port(), "/callback?code=1").status, 200);
    ASSERT_TRUE(again.answer());
    EXPECT_TRUE(redirect->last_problem().empty());
}

TEST(LoopbackRedirectPrompt, AFailingBrowserFunctionEndsThePromptAndClosesThePort) {
    LoopbackRedirectOptions options;
    options.open_browser = [](const std::string&) { throw std::runtime_error("no browser"); };
    const auto redirect = LoopbackRedirect::create(std::move(options));
    ASSERT_TRUE(redirect);
    RunningPrompt prompt((*redirect)->plain_prompt(), prompt_info(**redirect));
    const auto answer = prompt.answer();
    ASSERT_TRUE(answer);
    EXPECT_FALSE(answer->callback_url);
    EXPECT_FALSE((*redirect)->last_problem().empty());
    EXPECT_FALSE(connects((*redirect)->port()));
}

TEST(LoopbackRedirectPrompt, ASignInForAnotherRedirectUriIsNotWaitedFor) {
    Opened opened;
    const auto redirect = make_redirect(opened);
    ASSERT_TRUE(redirect);
    neomcp::AuthorizationPromptInfo info = prompt_info(*redirect);
    info.redirect_uri = "http://127.0.0.1:1/callback";
    RunningPrompt prompt(redirect->plain_prompt(), info);
    const auto answer = prompt.answer();
    ASSERT_TRUE(answer);
    EXPECT_FALSE(answer->callback_url);
    EXPECT_FALSE(redirect->last_problem().empty());
    EXPECT_TRUE(opened.urls().empty());
    EXPECT_FALSE(connects(redirect->port()));
}

TEST(LoopbackRedirectPrompt, OnlyOneSignInWaitsAtATime) {
    Opened opened;
    const auto redirect = make_redirect(opened);
    ASSERT_TRUE(redirect);
    RunningPrompt first(redirect->plain_prompt(), prompt_info(*redirect));
    ASSERT_TRUE(opened.wait(1));
    RunningPrompt second(redirect->plain_prompt(), prompt_info(*redirect));
    const auto refused = second.answer();
    ASSERT_TRUE(refused);
    EXPECT_FALSE(refused->callback_url);
    EXPECT_FALSE(redirect->last_problem().empty());
    // The first still waits, and is answered by the one request.
    EXPECT_TRUE(first.still_waiting());
    EXPECT_EQ(get(redirect->port(), "/callback?code=1").status, 200);
    EXPECT_TRUE(first.answer());
    EXPECT_EQ(opened.urls().size(), std::size_t{1});
}

}  // namespace
