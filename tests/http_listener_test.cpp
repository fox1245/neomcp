// Tests of the HTTP listener, over real sockets on the loopback interface (port 0: any free port).
//
// The HTTP library's client is used here, in the tests only, as the peer. Every wait is on an event a test controls
// (a promise, a counter with a condition variable); the bound on a wait only limits how long a broken build takes to
// fail. The few checks that something does NOT happen wait a short window and say so.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <httplib.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/http/body_stream.h"
#include "neomcp/http/listener.h"
#include "neomcp/http/message.h"
#include "neomcp/http/sse.h"

namespace {

using namespace std::chrono_literals;
using neomcp::http::BodyStream;
using neomcp::http::Handler;
using neomcp::http::Listener;
using neomcp::http::ListenerOptions;
using neomcp::http::Request;
using neomcp::http::Responder;
using neomcp::http::Response;

// Long enough for a loaded machine; reached only when something is broken.
constexpr std::chrono::milliseconds patience = 30s;
// How long stop() and a reported disconnect may take in a healthy run. Far below `patience`; generous for a sanitizer
// build on a loaded machine.
constexpr std::chrono::milliseconds promptly = 5s;

std::chrono::milliseconds since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
}

// A listener that has started, or an exception that fails the test.
struct Running {
    Running(ListenerOptions options, Handler handler) : listener(std::move(options), std::move(handler)) {
        auto started = listener.start();
        if (!started) throw std::runtime_error("start failed: " + started.error);
        port = *started.value;
    }
    Listener listener;
    std::uint16_t port = 0;
};

std::unique_ptr<Running> run(ListenerOptions options, Handler handler) {
    return std::make_unique<Running>(std::move(options), std::move(handler));
}

ListenerOptions defaults() { return ListenerOptions{}; }

std::unique_ptr<httplib::Client> make_client(std::uint16_t port) {
    auto client = std::make_unique<httplib::Client>("127.0.0.1", port);
    client->set_keep_alive(false);  // an idle kept connection would hold a worker of a small pool
    client->set_connection_timeout(30, 0);
    client->set_read_timeout(60, 0);
    client->set_write_timeout(60, 0);
    return client;
}

// A queue a test waits on.
template <class T>
class Mailbox {
public:
    void push(T value) {
        {
            std::lock_guard lock(mutex_);
            items_.push_back(std::move(value));
        }
        arrived_.notify_all();
    }
    std::optional<T> pop(std::chrono::milliseconds timeout = patience) {
        std::unique_lock lock(mutex_);
        if (!arrived_.wait_for(lock, timeout, [this] { return !items_.empty(); })) return std::nullopt;
        T value = std::move(items_.front());
        items_.pop_front();
        return value;
    }

private:
    std::mutex mutex_;
    std::condition_variable arrived_;
    std::deque<T> items_;
};

// A counter a test waits on.
class Counter {
public:
    void add() {
        {
            std::lock_guard lock(mutex_);
            ++value_;
        }
        changed_.notify_all();
    }
    int value() {
        std::lock_guard lock(mutex_);
        return value_;
    }
    bool wait_for_at_least(int n, std::chrono::milliseconds timeout = patience) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return value_ >= n; });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    int value_ = 0;
};

// A peer that reads a response as it arrives, on a thread of its own.
class StreamingClient {
public:
    explicit StreamingClient(std::uint16_t port, std::string path = "/", std::function<void()> on_finish = {})
        : client_(make_client(port)), path_(std::move(path)), on_finish_(std::move(on_finish)) {
        thread_ = std::thread([this] { read(); });
    }
    StreamingClient(const StreamingClient&) = delete;
    StreamingClient& operator=(const StreamingClient&) = delete;
    ~StreamingClient() {
        client_->stop();
        if (thread_.joinable()) thread_.join();
    }

    // Blocks until the status line and headers arrived (true), or the request failed first (false).
    bool wait_for_headers() {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, patience, [&] { return have_headers_ || finished_; });
        return have_headers_;
    }
    // Blocks until `predicate(body so far)` holds (true), or the response ended without that (false).
    bool wait_until(const std::function<bool(const std::string&)>& predicate,
                    std::chrono::milliseconds timeout = patience) {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, timeout, [&] { return predicate(body_) || finished_; });
        return predicate(body_);
    }
    bool wait_for_text(const std::string& text) {
        return wait_until([&](const std::string& body) { return body.find(text) != std::string::npos; });
    }
    // Blocks until the request ended; true when it ended with a complete response.
    bool wait_for_end() {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, patience, [&] { return finished_; });
        return finished_ && complete_;
    }
    // Without waiting.
    bool has_headers() {
        std::lock_guard lock(mutex_);
        return have_headers_;
    }
    bool finished() {
        std::lock_guard lock(mutex_);
        return finished_;
    }
    // Closes the connection from the peer's side.
    void disconnect() { client_->stop(); }

    std::string body() {
        std::lock_guard lock(mutex_);
        return body_;
    }
    int status() {
        std::lock_guard lock(mutex_);
        return status_;
    }
    std::string header(const std::string& name) {
        std::lock_guard lock(mutex_);
        return headers_.count(name) ? headers_.find(name)->second : std::string();
    }

private:
    void read() {
        auto result = client_->Get(
            path_,
            [this](const httplib::Response& response) {
                {
                    std::lock_guard lock(mutex_);
                    status_ = response.status;
                    headers_ = response.headers;
                    have_headers_ = true;
                }
                changed_.notify_all();
                return true;
            },
            [this](const char* data, std::size_t length) {
                {
                    std::lock_guard lock(mutex_);
                    body_.append(data, length);
                }
                changed_.notify_all();
                return true;
            });
        {
            std::lock_guard lock(mutex_);
            complete_ = static_cast<bool>(result);
            finished_ = true;
        }
        changed_.notify_all();
        if (on_finish_) on_finish_();
    }

    std::unique_ptr<httplib::Client> client_;
    std::string path_;
    std::function<void()> on_finish_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::string body_;
    int status_ = 0;
    httplib::Headers headers_;
    bool have_headers_ = false;
    bool finished_ = false;
    bool complete_ = false;
};

Response event_stream_response(std::shared_ptr<BodyStream> stream) {
    Response response = neomcp::http::make_response(200, "text/event-stream");
    response.stream = std::move(stream);
    return response;
}

// ---- Complete answers ------------------------------------------------------------------------------------------------

TEST(HttpListener, AnswersACompleteJsonResponse) {
    Mailbox<Request> seen;
    auto server = run(defaults(), [&](Request request, Responder responder) {
        seen.push(request);
        Response response = neomcp::http::make_response(200, "application/json", "{\"ok\":true}");
        response.headers.add("X-Custom", "one");
        response.headers.add("X-Custom", "two");
        responder.complete(std::move(response));
    });
    EXPECT_NE(server->port, 0);
    EXPECT_TRUE(server->listener.running());
    EXPECT_EQ(server->listener.port(), server->port);

    auto client = make_client(server->port);
    auto result = client->Post("/?mode=test&x=1", {{"X-Request", "abc"}}, "{\"hello\":1}", "application/json");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 200);
    EXPECT_EQ(result->body, "{\"ok\":true}");
    EXPECT_EQ(result->get_header_value("Content-Type"), "application/json");
    EXPECT_EQ(result->get_header_value_count("X-Custom"), 2u);
    EXPECT_EQ(result->get_header_value("Content-Length"), "11");

    auto request = seen.pop();
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->method, "POST");
    EXPECT_EQ(request->path, "/");
    EXPECT_EQ(request->query, "mode=test&x=1");
    EXPECT_EQ(request->body, "{\"hello\":1}");
    EXPECT_EQ(request->headers.get("x-request"), "abc");
    EXPECT_EQ(request->headers.get("CONTENT-TYPE"), "application/json");
    EXPECT_TRUE(request->headers.contains("Host"));
    ASSERT_TRUE(request->peer_address.has_value());
    EXPECT_EQ(*request->peer_address, "127.0.0.1");
}

TEST(HttpListener, AnEmptyAnswerKeepsItsStatus) {
    auto server = run(defaults(), [](Request, Responder responder) {
        responder.complete(neomcp::http::make_response(202));
    });
    auto client = make_client(server->port);
    auto result = client->Post("/", "x", "text/plain");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 202);
    EXPECT_TRUE(result->body.empty());
}

TEST(HttpListener, AStatusOutsideTheValidRangeBecomes500) {
    auto server = run(defaults(), [](Request, Responder responder) {
        responder.complete(neomcp::http::make_response(42));
    });
    auto result = make_client(server->port)->Get("/");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 500);
}

TEST(HttpListener, EveryMethodOnThePathGoesToTheHandler) {
    Mailbox<std::string> methods;
    auto server = run(defaults(), [&](Request request, Responder responder) {
        methods.push(request.method);
        Response response = neomcp::http::make_response(200, "text/plain", "seen " + request.method);
        response.headers.add("X-Method", request.method);
        responder.complete(std::move(response));
    });
    auto client = make_client(server->port);

    auto check = [&](const std::string& method, const httplib::Result& result) {
        SCOPED_TRACE(method);
        ASSERT_TRUE(result);
        EXPECT_EQ(result->status, 200);
        EXPECT_EQ(result->get_header_value("X-Method"), method);
        auto seen = methods.pop();
        ASSERT_TRUE(seen.has_value());
        EXPECT_EQ(*seen, method);
    };
    check("GET", client->Get("/"));
    check("POST", client->Post("/", "b", "text/plain"));
    check("PUT", client->Put("/", "b", "text/plain"));
    check("DELETE", client->Delete("/"));
    check("PATCH", client->Patch("/", "b", "text/plain"));
    check("OPTIONS", client->Options("/"));
    auto head = client->Head("/");
    check("HEAD", head);
    EXPECT_TRUE(head->body.empty());
}

TEST(HttpListener, AMethodTheLibraryDoesNotKnowIsAnswered400WithoutCallingTheHandler) {
    std::atomic<int> calls{0};
    auto server = run(defaults(), [&](Request, Responder responder) {
        ++calls;
        responder.complete(neomcp::http::make_response(200));
    });
    httplib::Request request;
    request.method = "PURGE";
    request.path = "/";
    auto result = make_client(server->port)->send(request);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 400);
    EXPECT_EQ(calls, 0);
}

TEST(HttpListener, OtherPathsGet404AndTheHandlerIsNotCalled) {
    std::atomic<int> calls{0};
    ListenerOptions options = defaults();
    options.path = "/mcp";
    auto server = run(options, [&](Request request, Responder responder) {
        ++calls;
        responder.complete(neomcp::http::make_response(200, "text/plain", request.path + "|" + request.query));
    });
    auto client = make_client(server->port);
    for (const char* path : {"/", "/mcp/", "/other", "/mcpx", "/mcp/sub", "/MCP"}) {
        auto result = client->Get(path);
        ASSERT_TRUE(result) << path;
        EXPECT_EQ(result->status, 404) << path;
    }
    auto posted = client->Post("/other", "x", "text/plain");
    ASSERT_TRUE(posted);
    EXPECT_EQ(posted->status, 404);
    auto deleted = client->Delete("/other");
    ASSERT_TRUE(deleted);
    EXPECT_EQ(deleted->status, 404);
    EXPECT_EQ(calls, 0);
    auto hit = client->Get("/mcp?a=b");
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->status, 200);
    EXPECT_EQ(hit->body, "/mcp|a=b");
}

TEST(HttpListener, ABodyOverTheBoundGets413AChunkedBody411AndTheHandlerIsNotCalled) {
    std::atomic<int> calls{0};
    ListenerOptions options = defaults();
    options.max_body_bytes = 1024;
    auto server = run(options, [&](Request request, Responder responder) {
        ++calls;
        responder.complete(neomcp::http::make_response(200, "text/plain", std::to_string(request.body.size())));
    });
    auto client = make_client(server->port);

    auto over = client->Post("/", std::string(1025, 'x'), "text/plain");
    ASSERT_TRUE(over);
    EXPECT_EQ(over->status, 413);
    EXPECT_EQ(calls, 0);

    auto exact = client->Post("/", std::string(1024, 'x'), "text/plain");
    ASSERT_TRUE(exact);
    EXPECT_EQ(exact->status, 200);
    EXPECT_EQ(exact->body, "1024");
    EXPECT_EQ(calls, 1);

    // A body without a declared length (the chunked coding) is refused before it is read, whatever its size: the
    // listener bounds the time a body takes only for one framed by Content-Length.
    for (const std::size_t total : {std::size_t{512}, std::size_t{4096}}) {
        SCOPED_TRACE(total);
        std::size_t sent = 0;
        auto chunked = make_client(server->port)->Post(
            "/", httplib::Headers(),
            [&](std::size_t, httplib::DataSink& sink) {
                if (sent >= total) {
                    sink.done();
                    return true;
                }
                const std::string piece(512, 'y');
                sink.write(piece.data(), piece.size());
                sent += piece.size();
                return true;
            },
            "text/plain");
        if (chunked) {  // the connection may also be cut before the answer is read
            EXPECT_EQ(chunked->status, 411);
        }
    }
    EXPECT_EQ(calls, 1);
}

TEST(HttpListener, AHandlerThatThrowsAnswers500) {
    auto server = run(defaults(), [](Request, Responder) { throw std::runtime_error("boom"); });
    auto result = make_client(server->port)->Get("/");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 500);
}

TEST(HttpListener, AResponderDestroyedWithoutCompletionAnswers500) {
    auto server = run(defaults(), [](Request, Responder) {});
    auto result = make_client(server->port)->Get("/");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 500);
}

// ---- Completion from another thread ----------------------------------------------------------------------------------

TEST(HttpListener, AHandlerMayCompleteItsResponderLaterFromAnotherThread) {
    Mailbox<Responder> parked;
    auto server = run(defaults(), [&](Request, Responder responder) { parked.push(std::move(responder)); });

    std::promise<int> status;
    auto answered = status.get_future();
    std::thread peer([&] {
        auto result = make_client(server->port)->Get("/");
        status.set_value(result ? result->status : -1);
    });

    auto responder = parked.pop();
    ASSERT_TRUE(responder.has_value());
    EXPECT_NE(answered.wait_for(0ms), std::future_status::ready);  // nothing was sent: the answer waits
    std::thread completer([r = *responder] { r.complete(neomcp::http::make_response(201, "text/plain", "later")); });
    ASSERT_EQ(answered.wait_for(patience), std::future_status::ready);
    EXPECT_EQ(answered.get(), 201);
    completer.join();
    peer.join();
}

// ---- Streams ---------------------------------------------------------------------------------------------------------

TEST(HttpListener, AStreamedAnswerDeliversChunksAsTheyAreProducedAndTheEndClosesTheResponse) {
    Mailbox<std::shared_ptr<BodyStream>> streams;
    auto server = run(defaults(), [&](Request, Responder responder) {
        auto stream = std::make_shared<BodyStream>();
        streams.push(stream);
        responder.complete(event_stream_response(stream));
    });
    StreamingClient client(server->port);
    auto stream = streams.pop();
    ASSERT_TRUE(stream.has_value());
    ASSERT_TRUE(client.wait_for_headers());
    EXPECT_EQ(client.status(), 200);
    EXPECT_EQ(client.header("Content-Type"), "text/event-stream");
    EXPECT_EQ(client.header("Cache-Control"), "no-store");
    EXPECT_EQ(client.header("X-Accel-Buffering"), "no");
    EXPECT_EQ(client.header("Transfer-Encoding"), "chunked");

    // Each chunk arrives before the next one is produced: the response is not buffered until its end.
    ASSERT_EQ((*stream)->append(neomcp::http::sse_event("one")), BodyStream::Append::accepted);
    ASSERT_TRUE(client.wait_for_text("data: one\n\n"));
    EXPECT_EQ(client.body(), "data: one\n\n");
    ASSERT_EQ((*stream)->append(neomcp::http::sse_event("two")), BodyStream::Append::accepted);
    ASSERT_TRUE(client.wait_for_text("data: two\n\n"));
    (*stream)->append(neomcp::http::sse_event("three"));
    (*stream)->end();
    EXPECT_TRUE(client.wait_for_end());
    EXPECT_EQ(client.body(), "data: one\n\ndata: two\n\ndata: three\n\n");
}

TEST(HttpListener, AStreamThatWasFilledAndEndedBeforeTheResponseIsCompletedIsDelivered) {
    auto server = run(defaults(), [](Request, Responder responder) {
        auto stream = std::make_shared<BodyStream>();
        stream->append("{\"a\":");
        stream->append("1}");
        stream->end();
        Response response = neomcp::http::make_response(200, "application/json");
        response.stream = stream;
        responder.complete(std::move(response));
    });
    StreamingClient client(server->port);
    EXPECT_TRUE(client.wait_for_end());
    EXPECT_EQ(client.body(), "{\"a\":1}");
    EXPECT_EQ(client.header("X-Accel-Buffering"), "");  // only event streams get the proxy headers
    EXPECT_EQ(client.header("Cache-Control"), "");
}

TEST(HttpListener, AnIdleEventStreamGetsKeepAliveComments) {
    ListenerOptions options = defaults();
    options.keep_alive_interval = 20ms;
    Mailbox<std::shared_ptr<BodyStream>> streams;
    auto server = run(options, [&](Request, Responder responder) {
        auto stream = std::make_shared<BodyStream>();
        streams.push(stream);
        responder.complete(event_stream_response(stream));
    });
    StreamingClient client(server->port);
    auto stream = streams.pop();
    ASSERT_TRUE(stream.has_value());
    const std::string comment = neomcp::http::sse_keep_alive();
    ASSERT_TRUE(client.wait_until([&](const std::string& body) {
        std::size_t count = 0;
        for (std::size_t at = body.find(comment); at != std::string::npos; at = body.find(comment, at + 1)) ++count;
        return count >= 3;
    }));
    // Real data goes between the comments.
    (*stream)->append(neomcp::http::sse_event("payload"));
    ASSERT_TRUE(client.wait_for_text("data: payload\n\n"));
    (*stream)->end();
    EXPECT_TRUE(client.wait_for_end());
    const std::string body = client.body();
    EXPECT_EQ(body.rfind(comment, 0), 0u);  // the stream starts with a comment
}

TEST(HttpListener, AStreamOfAnotherTypeGetsNoKeepAliveComments) {
    ListenerOptions options = defaults();
    options.keep_alive_interval = 10ms;
    options.disconnect_poll_interval = 10ms;
    Mailbox<std::shared_ptr<BodyStream>> streams;
    auto server = run(options, [&](Request, Responder responder) {
        auto stream = std::make_shared<BodyStream>();
        streams.push(stream);
        Response response = neomcp::http::make_response(200, "application/json");
        response.stream = stream;
        responder.complete(std::move(response));
    });
    StreamingClient client(server->port);
    auto stream = streams.pop();
    ASSERT_TRUE(stream.has_value());
    (*stream)->append("[");
    ASSERT_TRUE(client.wait_for_text("["));
    // Nothing may arrive in many keep-alive intervals (this is the one wait on a window in this file).
    EXPECT_FALSE(client.wait_until([](const std::string& body) { return body != "["; }, 300ms));
    (*stream)->append("]");
    (*stream)->end();
    EXPECT_TRUE(client.wait_for_end());
    EXPECT_EQ(client.body(), "[]");
}

// ---- A peer that goes away ------------------------------------------------------------------------------------------

// A handler that gives every request an event stream and reports, through `closed`, when a stream's consumer closed.
struct StreamHost {
    Mailbox<std::shared_ptr<BodyStream>> streams;
    Counter closed;
    Handler handler(const char* content_type = "text/event-stream") {
        return [this, content_type](Request, Responder responder) {
            auto stream = std::make_shared<BodyStream>();
            stream->on_close([this] { closed.add(); });
            streams.push(stream);
            Response response = neomcp::http::make_response(200, content_type);
            response.stream = stream;
            responder.complete(std::move(response));
        };
    }
};

TEST(HttpListener, AClientThatGoesAwayFromAnIdleStreamIsReportedWithinAPollInterval) {
    ListenerOptions options = defaults();
    options.disconnect_poll_interval = 25ms;
    options.keep_alive_interval = 1h;  // no write will reveal the disconnect: only the poll can
    StreamHost host;
    auto server = run(options, host.handler());
    StreamingClient client(server->port);
    ASSERT_TRUE(client.wait_for_headers());
    ASSERT_TRUE(host.streams.pop().has_value());
    EXPECT_EQ(host.closed.value(), 0);

    const auto start = std::chrono::steady_clock::now();
    client.disconnect();
    ASSERT_TRUE(host.closed.wait_for_at_least(1, promptly));
    std::cout << "[measure] idle stream, disconnect reported after " << since(start).count() << " ms (poll 25 ms)\n";
    EXPECT_EQ(host.closed.value(), 1);
}

TEST(HttpListener, AClientThatGoesAwayFromAnIdleStreamOfAnotherTypeIsReportedToo) {
    ListenerOptions options = defaults();
    options.disconnect_poll_interval = 25ms;
    StreamHost host;
    auto server = run(options, host.handler("application/json"));
    StreamingClient client(server->port);
    ASSERT_TRUE(client.wait_for_headers());
    client.disconnect();
    EXPECT_TRUE(host.closed.wait_for_at_least(1, promptly));
}

TEST(HttpListener, AClientThatGoesAwayFromABusyStreamMakesLaterAppendsFail) {
    StreamHost host;
    auto server = run(defaults(), host.handler());
    StreamingClient client(server->port);
    auto stream = host.streams.pop();
    ASSERT_TRUE(stream.has_value());
    (*stream)->append(neomcp::http::sse_event("first"));
    ASSERT_TRUE(client.wait_for_text("first"));
    client.disconnect();
    // The producer keeps appending; the consumer closes the stream, and the producer sees it in the return value.
    const auto start = std::chrono::steady_clock::now();
    bool refused = false;
    while (since(start) < promptly) {
        if ((*stream)->append(neomcp::http::sse_event("more")) == BodyStream::Append::closed) {
            refused = true;
            break;
        }
        std::this_thread::sleep_for(1ms);  // pacing a producer; not the mechanism the test waits on
    }
    EXPECT_TRUE(refused);
    EXPECT_TRUE(host.closed.wait_for_at_least(1, promptly));
    std::cout << "[measure] busy stream, disconnect reported after " << since(start).count() << " ms\n";
}

// ---- A disconnect before the answer ----------------------------------------------------------------------------------

TEST(HttpListener, AClientThatGoesAwayBeforeTheAnswerIsReportedThroughTheResponderAndALateAnswerIsDropped) {
    ListenerOptions options = defaults();
    options.disconnect_poll_interval = 25ms;
    Mailbox<Responder> kept;
    Counter told;
    std::promise<std::thread::id> reporter;
    auto server = run(options, [&](Request, Responder responder) {
        responder.on_disconnect([&] {
            reporter.set_value(std::this_thread::get_id());
            told.add();
        });
        kept.push(responder);
    });
    StreamingClient client(server->port);
    auto responder = kept.pop();
    ASSERT_TRUE(responder.has_value());
    EXPECT_EQ(told.value(), 0);

    const auto start = std::chrono::steady_clock::now();
    client.disconnect();
    ASSERT_TRUE(told.wait_for_at_least(1, promptly));
    std::cout << "[measure] pending request, disconnect reported after " << since(start).count()
              << " ms (poll 25 ms)\n";
    auto thread = reporter.get_future();
    ASSERT_EQ(thread.wait_for(patience), std::future_status::ready);
    EXPECT_NE(thread.get(), std::this_thread::get_id());  // on a thread of the listener
    EXPECT_TRUE(responder->completed());
    EXPECT_FALSE(responder->complete(neomcp::http::make_response(200, "application/json", "{}")));
    EXPECT_EQ(told.value(), 1);
}

TEST(HttpListener, TheDisconnectCallbackMayCallBackIntoTheResponderAndTheListener) {
    ListenerOptions options = defaults();
    options.disconnect_poll_interval = 25ms;
    Counter arrived;
    Counter told;
    std::atomic<bool> late_answer_taken{true};
    std::atomic<bool> running_inside{false};
    std::atomic<Listener*> listener{nullptr};
    auto server = run(options, [&](Request, Responder responder) {
        // The callback holds a copy, so the request stays open until the disconnect.
        responder.on_disconnect([&, responder] {
            late_answer_taken = responder.complete(neomcp::http::make_response(200));
            running_inside = listener.load()->running();  // takes a lock of the listener: none may be held here
            told.add();
        });
        arrived.add();
    });
    listener = &server->listener;
    {
        StreamingClient client(server->port);
        ASSERT_TRUE(arrived.wait_for_at_least(1));
        client.disconnect();
        ASSERT_TRUE(told.wait_for_at_least(1, promptly));
    }
    EXPECT_FALSE(late_answer_taken.load());
    EXPECT_TRUE(running_inside.load());
}

TEST(HttpListener, ARequestAnsweredBeforeTheClientLeavesIsNotReportedAsADisconnect) {
    ListenerOptions options = defaults();
    options.disconnect_poll_interval = 25ms;
    Counter told;
    auto server = run(options, [&](Request, Responder responder) {
        responder.on_disconnect([&] { told.add(); });
        responder.complete(neomcp::http::make_response(200, "application/json", "{}"));
    });
    auto result = make_client(server->port)->Post("/", "{}", "application/json");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 200);
    // Nothing should happen; a window of many poll intervals shows it does not.
    EXPECT_FALSE(told.wait_for_at_least(1, 250ms));
}

TEST(HttpListener, AClientThatWaitsIsNotTakenForGoneAndGetsTheAnswer) {
    ListenerOptions options = defaults();
    options.disconnect_poll_interval = 10ms;
    Mailbox<Responder> kept;
    Counter told;
    auto server = run(options, [&](Request, Responder responder) {
        responder.on_disconnect([&] { told.add(); });
        kept.push(responder);
    });
    auto answer = std::async(std::launch::async, [&] {
        return make_client(server->port)->Post("/", "{\"a\":1}", "application/json");
    });
    auto responder = kept.pop();
    ASSERT_TRUE(responder.has_value());
    // The worker looks at the connection about 25 times in this window; the client is still there each time.
    EXPECT_FALSE(told.wait_for_at_least(1, 250ms));
    EXPECT_TRUE(responder->complete(neomcp::http::make_response(200, "application/json", "{\"ok\":true}")));
    auto result = answer.get();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 200);
    EXPECT_EQ(result->body, "{\"ok\":true}");
    EXPECT_EQ(told.value(), 0);
}

TEST(HttpListener, AHeadRequestToAStreamAnswersWithoutABodyAndClosesTheStream) {
    StreamHost host;
    auto server = run(defaults(), host.handler());
    auto result = make_client(server->port)->Head("/");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 200);
    EXPECT_TRUE(result->body.empty());
    EXPECT_TRUE(host.closed.wait_for_at_least(1, promptly));
}

// ---- Stopping --------------------------------------------------------------------------------------------------------

TEST(HttpListener, StopEndsEveryOpenStreamAndReturnsWithinABound) {
    constexpr int streams = 8;
    ListenerOptions options = defaults();
    options.worker_threads = streams;
    options.max_worker_threads = streams;
    options.disconnect_poll_interval = 10s;  // stop() must not wait for a worker's next look at its stream
    StreamHost host;
    auto server = run(options, host.handler());

    std::vector<std::unique_ptr<StreamingClient>> clients;
    for (int i = 0; i < streams; ++i) clients.push_back(std::make_unique<StreamingClient>(server->port));
    for (auto& client : clients) ASSERT_TRUE(client->wait_for_headers());
    EXPECT_EQ(host.closed.value(), 0);

    const auto start = std::chrono::steady_clock::now();
    server->listener.stop();
    const auto elapsed = since(start);
    std::cout << "[measure] stop() with " << streams << " open streams took " << elapsed.count() << " ms\n";
    EXPECT_LT(elapsed, promptly);
    EXPECT_EQ(host.closed.value(), streams);  // every producer was told, before stop() returned
    EXPECT_FALSE(server->listener.running());
    for (auto& client : clients) EXPECT_FALSE(client->wait_for_end());  // cut, not ended cleanly
}

TEST(HttpListener, StopAnswers503ToRequestsWhoseResponderWasNeverCompleted) {
    Mailbox<Responder> parked;
    auto server = run(defaults(), [&](Request, Responder responder) { parked.push(std::move(responder)); });
    std::promise<int> status;
    auto answered = status.get_future();
    std::thread peer([&] {
        auto result = make_client(server->port)->Get("/");
        status.set_value(result ? result->status : -1);
    });
    auto responder = parked.pop();  // the handler ran; its responder is alive in this test and never completed
    ASSERT_TRUE(responder.has_value());

    const auto start = std::chrono::steady_clock::now();
    server->listener.stop();
    const auto elapsed = since(start);
    std::cout << "[measure] stop() with an uncompleted responder took " << elapsed.count() << " ms\n";
    EXPECT_LT(elapsed, promptly);
    ASSERT_EQ(answered.wait_for(patience), std::future_status::ready);
    EXPECT_EQ(answered.get(), 503);
    EXPECT_FALSE(responder->complete(neomcp::http::make_response(200)));  // too late: and nothing is touched
    peer.join();
}

TEST(HttpListener, StopIsIdempotentAndAStoppedListenerDoesNotStartAgain) {
    auto server = run(defaults(), [](Request, Responder responder) { responder.complete(neomcp::http::make_response(200)); });
    server->listener.stop();
    server->listener.stop();
    EXPECT_FALSE(server->listener.running());
    EXPECT_EQ(server->listener.port(), 0);
    auto again = server->listener.start();
    EXPECT_FALSE(again);
    EXPECT_FALSE(again.error.empty());

    Listener never_started(defaults(), [](Request, Responder) {});
    never_started.stop();  // stopping a listener that never started is harmless
    EXPECT_FALSE(never_started.start());
}

TEST(HttpListener, ADestroyedListenerStopsServing) {
    std::uint16_t port = 0;
    {
        auto server = run(defaults(), [](Request, Responder responder) { responder.complete(neomcp::http::make_response(200)); });
        port = server->port;
        EXPECT_TRUE(make_client(port)->Get("/"));
    }
    auto client = make_client(port);
    client->set_connection_timeout(2, 0);
    EXPECT_FALSE(client->Get("/"));
}

// ---- Starting --------------------------------------------------------------------------------------------------------

TEST(HttpListener, StartReportsAPortInUseAsAnErrorValue) {
    auto first = run(defaults(), [](Request, Responder responder) { responder.complete(neomcp::http::make_response(200)); });
    ListenerOptions options = defaults();
    options.port = first->port;
    Listener second(options, [](Request, Responder) {});
    auto started = second.start();  // no exception
    EXPECT_FALSE(started);
    EXPECT_NE(started.error.find(std::to_string(first->port)), std::string::npos) << started.error;
    EXPECT_FALSE(second.running());
    // The first one still serves.
    auto result = make_client(first->port)->Get("/");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 200);
}

TEST(HttpListener, TwoListenersOnPortZeroGetDifferentPortsAndAReleasedPortCanBeBoundAgain) {
    auto handler = [](Request, Responder responder) { responder.complete(neomcp::http::make_response(200)); };
    auto first = run(defaults(), handler);
    auto second = run(defaults(), handler);
    EXPECT_NE(first->port, second->port);
    const std::uint16_t released = first->port;
    first->listener.stop();

    ListenerOptions options = defaults();
    options.port = released;
    auto third = run(options, handler);
    EXPECT_EQ(third->port, released);
    EXPECT_TRUE(make_client(released)->Get("/"));
}

TEST(HttpListener, StartRejectsOptionsOutOfRange) {
    struct Case {
        const char* what;
        std::function<void(ListenerOptions&)> change;
        const char* expect_in_message;
    };
    const std::vector<Case> cases = {
        {"empty bind address", [](ListenerOptions& o) { o.bind_address.clear(); }, "bind_address"},
        {"empty path", [](ListenerOptions& o) { o.path.clear(); }, "path"},
        {"path without a slash", [](ListenerOptions& o) { o.path = "mcp"; }, "path"},
        {"path with a dot", [](ListenerOptions& o) { o.path = "/v1.0/mcp"; }, "path"},
        {"path with a parameter", [](ListenerOptions& o) { o.path = "/users/:id"; }, "path"},
        {"path with a wildcard", [](ListenerOptions& o) { o.path = "/a/*"; }, "path"},
        {"path with a query", [](ListenerOptions& o) { o.path = "/a?b"; }, "path"},
        {"no body at all", [](ListenerOptions& o) { o.max_body_bytes = 0; }, "max_body_bytes"},
        {"no workers", [](ListenerOptions& o) { o.worker_threads = 0; }, "worker_threads"},
        {"maximum below base", [](ListenerOptions& o) { o.worker_threads = 4; o.max_worker_threads = 3; }, "max_worker_threads"},
        {"no queue", [](ListenerOptions& o) { o.max_queued_connections = 0; }, "max_queued_connections"},
        {"zero read timeout", [](ListenerOptions& o) { o.read_timeout = 0ms; }, "read_timeout"},
        {"zero body timeout", [](ListenerOptions& o) { o.body_timeout = 0ms; }, "body_timeout"},
        {"zero write timeout", [](ListenerOptions& o) { o.write_timeout = 0ms; }, "write_timeout"},
        {"zero idle connection timeout", [](ListenerOptions& o) { o.idle_connection_timeout = 0s; }, "idle_connection_timeout"},
        {"zero keep-alive interval", [](ListenerOptions& o) { o.keep_alive_interval = 0ms; }, "keep_alive_interval"},
        {"zero poll interval", [](ListenerOptions& o) { o.disconnect_poll_interval = 0ms; }, "disconnect_poll_interval"},
        {"zero worker idle timeout", [](ListenerOptions& o) { o.worker_idle_timeout = 0s; }, "worker_idle_timeout"},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.what);
        ListenerOptions options = defaults();
        c.change(options);
        Listener listener(options, [](Request, Responder) {});
        auto started = listener.start();
        EXPECT_FALSE(started);
        EXPECT_NE(started.error.find(c.expect_in_message), std::string::npos) << started.error;
    }
}

TEST(HttpListener, StartRejectsAnEmptyHandlerAndAnUnbindableAddress) {
    Listener no_handler(defaults(), Handler{});
    EXPECT_FALSE(no_handler.start());

    ListenerOptions options = defaults();
    options.bind_address = "203.0.113.77";  // a documentation address: not assigned to this machine
    Listener elsewhere(options, [](Request, Responder) {});
    auto started = elsewhere.start();
    EXPECT_FALSE(started);
    EXPECT_FALSE(started.error.empty());
}

// ---- More streams than workers ---------------------------------------------------------------------------------------

// What the options say when open streams outnumber the workers: with `worker_threads` = `max_worker_threads` = 2 and
// one queued connection, two streams hold both workers; a third connection waits unanswered in the queue and is
// served as soon as a stream ends; a fourth connection (the queue is full) is closed at once.
TEST(HttpListener, MoreStreamsThanWorkersQueueOneConnectionAndRefuseTheNext) {
    ListenerOptions options = defaults();
    options.worker_threads = 2;
    options.max_worker_threads = 2;
    options.max_queued_connections = 1;
    options.disconnect_poll_interval = 25ms;
    StreamHost host;
    auto server = run(options, host.handler());

    Counter freed;  // counts the first two connections as they end
    StreamingClient first(server->port, "/", [&] { freed.add(); });
    StreamingClient second(server->port, "/", [&] { freed.add(); });
    ASSERT_TRUE(first.wait_for_headers());
    ASSERT_TRUE(second.wait_for_headers());
    auto stream_one = host.streams.pop();
    auto stream_two = host.streams.pop();
    ASSERT_TRUE(stream_one && stream_two);

    // Two more connections: one is queued, the other is refused. Which one is which depends on the order in which the
    // listener accepted them, so the test counts outcomes.
    Counter ended;
    StreamingClient third(server->port, "/", [&] { ended.add(); });
    StreamingClient fourth(server->port, "/", [&] { ended.add(); });
    ASSERT_TRUE(ended.wait_for_at_least(1));  // the refused connection is closed without an answer
    EXPECT_NE(third.finished(), fourth.finished());
    EXPECT_FALSE(third.has_headers());  // the queued connection has no answer while both workers are busy
    EXPECT_FALSE(fourth.has_headers());

    (*stream_one)->end();  // frees a worker: the queued connection is served now
    ASSERT_TRUE(freed.wait_for_at_least(1));
    auto stream_late = host.streams.pop();
    ASSERT_TRUE(stream_late.has_value());
    (*stream_late)->end();
    (*stream_two)->end();
    EXPECT_TRUE(first.wait_for_end());
    EXPECT_TRUE(second.wait_for_end());
    ASSERT_TRUE(ended.wait_for_at_least(2));
    // Exactly one of the two late connections got an answer, the other was refused.
    EXPECT_NE(third.wait_for_end(), fourth.wait_for_end());
}

// A burst of connections that fits in the workers plus the waiting bound is served whole, even when it arrives faster
// than the idle workers can take their connections. A fresh listener for each round keeps connections that are still
// closing from an earlier round out of the count.
TEST(HttpListener, ABurstThatFitsTheWorkersAndTheQueueIsNeverRefused) {
    constexpr int rounds = 20;
    constexpr int burst = 4;
    int refused = 0;
    for (int round = 0; round < rounds; ++round) {
        ListenerOptions options = defaults();
        options.worker_threads = 1;
        options.max_worker_threads = 2;
        options.max_queued_connections = 2;  // up to 4 open connections
        auto server = run(options, [](Request, Responder responder) { responder.complete(neomcp::http::make_response(200)); });
        std::promise<void> go;
        const std::shared_future<void> start = go.get_future().share();
        std::atomic<int> failed{0};
        std::vector<std::thread> peers;
        for (int i = 0; i < burst; ++i) {
            peers.emplace_back([&] {
                auto client = make_client(server->port);
                start.wait();
                auto result = client->Get("/");
                if (!result || result->status != 200) ++failed;
            });
        }
        go.set_value();
        for (auto& peer : peers) peer.join();
        refused += failed;
    }
    EXPECT_EQ(refused, 0);
}

}  // namespace
