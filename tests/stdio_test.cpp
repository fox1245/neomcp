// Tests of the stdio transport.
//
// The transport is driven over byte streams the tests control: a blocking queue of chunks as input and a collector as
// output, and, where the platform has pipes, over real descriptors. Every wait is on an event a test controls; the
// bound on a wait only limits how long a broken build takes to fail. The tests of the drain at the end of input
// choose a drain timeout that only a broken build reaches, or one so short that reaching it is the behaviour tested.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include "neomcp/server.h"
#include "neomcp/stdio.h"
#include "neomcp/testing/peer.h"
#include "stdio/fd_streams.h"
#include "stdio/line_splitter.h"

namespace {

using namespace std::chrono_literals;
using neomcp::json;
using neomcp::testing::Peer;
namespace profile = neomcp::profile;

// Long enough for a sanitizer build on a loaded machine; reached only when something is broken.
constexpr std::chrono::milliseconds patience = 30s;

profile::ProfilePtr builtin(const std::string& revision) {
    profile::ProfilePtr found = profile::builtin(revision);
    if (!found) throw std::logic_error("no built-in profile " + revision);
    return found;
}

// A flag the test raises once.
class Event {
public:
    void set() {
        const std::lock_guard<std::mutex> lock(mutex_);
        set_ = true;
        changed_.notify_all();
    }
    bool wait(std::chrono::milliseconds bound) {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, bound, [&] { return set_; });
    }
    // Whether it was raised; false when the wait ended because stop was requested.
    bool wait(std::stop_token token) {
        // Registered before the lock is taken: the callback runs at once when stop was requested already.
        std::stop_callback wake(token, [this] {
            const std::lock_guard<std::mutex> lock(mutex_);
            changed_.notify_all();
        });
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] { return set_ || token.stop_requested(); });
        return set_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool set_ = false;
};

// Input that blocks until the test pushes bytes or ends it.
class QueueReader final : public neomcp::ByteReader {
public:
    void push(std::string bytes) {
        const std::lock_guard<std::mutex> lock(mutex_);
        chunks_.push_back(std::move(bytes));
        changed_.notify_all();
    }
    void end() {
        const std::lock_guard<std::mutex> lock(mutex_);
        ended_ = true;
        changed_.notify_all();
    }
    bool interrupted() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return interrupted_;
    }

    std::size_t read(char* buffer, std::size_t capacity) override {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] { return !chunks_.empty() || ended_ || interrupted_; });
        if (interrupted_ || chunks_.empty()) return 0;
        std::string& front = chunks_.front();
        const std::size_t count = std::min(capacity, front.size());
        front.copy(buffer, count);
        front.erase(0, count);
        if (front.empty()) chunks_.pop_front();
        return count;
    }
    void interrupt() noexcept override {
        const std::lock_guard<std::mutex> lock(mutex_);
        interrupted_ = true;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::string> chunks_;
    bool ended_ = false;
    bool interrupted_ = false;
};

// Output that collects what is written and can start to fail.
class CollectingWriter final : public neomcp::ByteWriter {
public:
    bool write(std::string_view bytes) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        ++writes_;
        if (fail_ || (fail_from_ && writes_ >= *fail_from_)) return false;
        text_.append(bytes);
        changed_.notify_all();
        return true;
    }
    // From the n-th write on (counted from 1), writes fail.
    void fail_from(std::size_t n) {
        const std::lock_guard<std::mutex> lock(mutex_);
        fail_from_ = n;
    }
    void fail_now() {
        const std::lock_guard<std::mutex> lock(mutex_);
        fail_ = true;
    }

    std::string text() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return text_;
    }
    // The complete lines written so far, parsed.
    std::vector<json> messages() {
        const std::string all = text();
        std::vector<json> out;
        std::size_t start = 0;
        for (std::size_t end = all.find('\n'); end != std::string::npos; end = all.find('\n', start)) {
            out.push_back(json::parse(all.substr(start, end - start)));
            start = end + 1;
        }
        return out;
    }
    bool wait_for_lines(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, patience, [&] {
            return static_cast<std::size_t>(std::count(text_.begin(), text_.end(), '\n')) >= count;
        });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::string text_;
    std::size_t writes_ = 0;
    std::optional<std::size_t> fail_from_;
    bool fail_ = false;
};

// Serves on its own thread. The destructor ends the input, so that a test that failed early does not hang.
class Serving {
public:
    Serving(neomcp::Server& server, QueueReader& input, CollectingWriter& output, neomcp::StdioOptions options)
        : input_(input) {
        std::future<int> result = done_.get_future();
        result_ = std::move(result);
        thread_ = std::thread([this, &server, &input, &output, options] {
            try {
                done_.set_value(neomcp::serve_streams(server, input, output, options));
            } catch (...) {
                done_.set_exception(std::current_exception());
            }
        });
    }
    ~Serving() {
        input_.end();
        if (thread_.joinable()) thread_.join();
    }
    Serving(const Serving&) = delete;
    Serving& operator=(const Serving&) = delete;

    // The value serve_streams returned; fails the test when it does not return within the patience.
    std::optional<int> result() {
        if (result_.wait_for(patience) != std::future_status::ready) return std::nullopt;
        return result_.get();
    }

private:
    QueueReader& input_;
    std::promise<int> done_;
    std::future<int> result_;
    std::thread thread_;
};

neomcp::StdioOptions quiet() {
    neomcp::StdioOptions options;
    options.diagnostics = false;
    return options;
}

neomcp::ServerOptions named(std::string name) {
    neomcp::ServerOptions options;
    options.name = std::move(name);
    options.version = "1.0.0";
    return options;
}

std::string line_of(const json& message) { return message.dump() + "\n"; }

json ping(int id) { return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", "ping"}}; }
json pong(int id) { return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", json::object()}}; }

int code_of(const json& message) { return message.at("error").at("code").get<int>(); }

// A server with a few tools: one that answers at once, one that holds the call until the test lets go, and one that
// reports what it saw of its stop token.
struct Fixture {
    Fixture() : server(named("stdio-test")) {
        server.tool("hello", "Answer at once", [] { return std::string("hello"); });
        server.tool("gated", "Wait for the test, or for stop", [this](neomcp::Context& context) {
            entered.set();
            const bool released = gate.wait(context.stop_token());
            if (!released) stopped.set();
            return std::string(released ? "released" : "stopped");
        });
    }
    Event entered;
    Event gate;
    Event stopped;
    neomcp::Server server;
};

json legacy_initialize(int id) { return Peer::initialize_request(*builtin("2025-11-25"), id); }
json legacy_call(int id, const std::string& tool) {
    return Peer::make_request(*builtin("2025-11-25"), id, "tools/call", {{"name", tool}});
}
json modern_call(int id, const std::string& tool) {
    return Peer::make_request(*builtin("2026-07-28"), id, "tools/call", {{"name", tool}});
}

std::string text_of(const json& response) {
    return response.at("result").at("content").at(0).at("text").get<std::string>();
}

// ---------------------------------------------------------------------------------------------------------------
// The line splitter

std::vector<std::string> split(std::size_t limit, const std::vector<std::string>& chunks, bool finish) {
    neomcp::detail::LineSplitter splitter(limit);
    std::vector<std::string> lines;
    const auto keep = [&](std::string_view line) { lines.emplace_back(line); };
    for (const auto& chunk : chunks) splitter.feed(chunk, keep);
    if (finish) splitter.finish(keep);
    return lines;
}

std::vector<std::string> by_byte(const std::string& input) {
    std::vector<std::string> chunks;
    for (const char c : input) chunks.emplace_back(1, c);
    return chunks;
}

TEST(LineSplitter, TerminatorsAndEmptyLines) {
    const std::string input = "one\r\ntwo\n\n\r\nthree\r\r\nfour";
    const std::vector<std::string> expected{"one", "two", "three\r"};
    EXPECT_EQ(split(1000, {input}, false), expected);
    EXPECT_EQ(split(1000, by_byte(input), false), expected);
    // The last line has no terminator: it is a line at the end of input and nothing before it.
    EXPECT_EQ(split(1000, {input}, true), (std::vector<std::string>{"one", "two", "three\r", "four"}));
    EXPECT_EQ(split(1000, by_byte(input), true), (std::vector<std::string>{"one", "two", "three\r", "four"}));
    EXPECT_TRUE(split(1000, {"\r"}, true).empty());
    EXPECT_TRUE(split(1000, {""}, true).empty());
}

// The end of the input ends the last line like a line feed does, so one carriage return before it is dropped in
// the same way, and the bound is measured without it.
TEST(LineSplitter, TheEndOfTheInputEndsTheLastLineLikeALineFeedDoes) {
    EXPECT_EQ(split(1000, {"a\nb\r"}, true), (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(split(1000, {"b\r\r"}, true), (std::vector<std::string>{"b\r"}));
    EXPECT_EQ(split(1000, {"b\r\n"}, true), (std::vector<std::string>{"b"}));
    EXPECT_EQ(split(1000, {"b\r"}, false), std::vector<std::string>{});  // not finished: nothing is delivered
    EXPECT_EQ(split(1000, {"b\r", "\r"}, true), (std::vector<std::string>{"b\r"}));
    EXPECT_EQ(split(1000, {"b", "\r"}, true), (std::vector<std::string>{"b"}));
    EXPECT_EQ(split(1000, by_byte("a\r\nb\r"), true), (std::vector<std::string>{"a", "b"}));
    // A lone carriage return is an empty line, as a lone "\r\n" is.
    EXPECT_TRUE(split(1000, {"x\n", "\r"}, true) == std::vector<std::string>{"x"});
    constexpr std::size_t limit = 10;
    const std::string at_limit(limit, 'x');
    const std::string over_limit(limit + 1, 'y');
    EXPECT_EQ(split(limit, {at_limit + "\r"}, true), (std::vector<std::string>{at_limit}));
    EXPECT_EQ(split(limit, {over_limit + "\r"}, true), (std::vector<std::string>{over_limit}));
}

TEST(LineSplitter, ALimitThatCannotBeExceededKeepsEveryLine) {
    const std::size_t no_limit = std::numeric_limits<std::size_t>::max();
    EXPECT_EQ(split(no_limit, {"ab\ncd\n"}, false), (std::vector<std::string>{"ab", "cd"}));
    EXPECT_EQ(split(no_limit, {"tail"}, true), (std::vector<std::string>{"tail"}));
}

TEST(LineSplitter, ALineIsWhatLiesBetweenTerminatorsWhateverTheChunks) {
    const std::string input = "a\nbb\r\n\nccc\n";
    const std::vector<std::string> expected{"a", "bb", "ccc"};
    for (std::size_t step = 1; step <= input.size(); ++step) {
        std::vector<std::string> chunks;
        for (std::size_t i = 0; i < input.size(); i += step) chunks.push_back(input.substr(i, step));
        EXPECT_EQ(split(100, chunks, true), expected) << "chunks of " << step;
    }
}

TEST(LineSplitter, TheBoundIsMeasuredWithoutTheCarriageReturn) {
    constexpr std::size_t limit = 10;
    const std::string at_limit(limit, 'x');
    const std::string over_limit(limit + 1, 'y');
    // At the limit, with or without a carriage return: delivered whole.
    EXPECT_EQ(split(limit, {at_limit + "\n"}, false), (std::vector<std::string>{at_limit}));
    EXPECT_EQ(split(limit, {at_limit + "\r\n"}, false), (std::vector<std::string>{at_limit}));
    // One byte over: delivered with limit + 1 bytes, which tells the receiver it is too long.
    EXPECT_EQ(split(limit, {over_limit + "\n"}, false), (std::vector<std::string>{over_limit}));
    EXPECT_EQ(split(limit, {over_limit + "\r\n"}, false), (std::vector<std::string>{over_limit}));
    // A carriage return in the middle is not a terminator, and does not hide the excess behind it.
    const auto inner = split(limit, {at_limit + "\ry\n"}, false);
    ASSERT_EQ(inner.size(), 1u);
    EXPECT_EQ(inner[0].size(), limit + 1);
    EXPECT_NE(inner[0].back(), '\n');
}

TEST(LineSplitter, ALongLineIsSkippedWithoutBeingStored) {
    constexpr std::size_t limit = 1000;
    neomcp::detail::LineSplitter splitter(limit);
    std::vector<std::size_t> sizes;
    const auto keep = [&](std::string_view line) { sizes.push_back(line.size()); };
    const std::string chunk(4096, 'z');
    constexpr std::size_t chunks = 5 * 256;  // 5 MiB in one line
    for (std::size_t i = 0; i < chunks; ++i) splitter.feed(chunk, keep);
    EXPECT_TRUE(sizes.empty());
    // Whatever the growth policy of the string, it stays within a small multiple of the bound.
    EXPECT_LE(splitter.capacity(), 2 * (limit + 1) + 64);
    splitter.feed("\nshort\n", keep);
    EXPECT_EQ(sizes, (std::vector<std::size_t>{limit + 1, 5}));
    EXPECT_LE(splitter.capacity(), 2 * (limit + 1) + 64);
}

// ---------------------------------------------------------------------------------------------------------------
// Framing over the stream pair

TEST(Framing, CarriageReturnsEmptyLinesSeveralMessagesAndSplitMessages) {
    Fixture f;
    QueueReader input;
    CollectingWriter output;
    Serving run(f.server, input, output, quiet());

    const std::string second = ping(2).dump();
    // Several messages in one chunk, with an empty line and a carriage return; the second message is cut in two.
    input.push(ping(1).dump() + "\r\n\n\r\n" + second.substr(0, 10));
    ASSERT_TRUE(output.wait_for_lines(1));
    EXPECT_EQ(output.messages(), (std::vector<json>{pong(1)}));
    input.push(second.substr(10) + "\n" + ping(3).dump() + "\n");
    ASSERT_TRUE(output.wait_for_lines(3));
    // A last message without a terminator counts at the end of input.
    input.push(ping(4).dump());
    input.end();
    EXPECT_EQ(run.result(), 0);
    EXPECT_EQ(output.messages(), (std::vector<json>{pong(1), pong(2), pong(3), pong(4)}));
    // Each output message is one line ending in a line feed, with no carriage return.
    const std::string text = output.text();
    EXPECT_EQ(text.back(), '\n');
    EXPECT_EQ(text.find('\r'), std::string::npos);
}

TEST(Framing, EveryChunkSizeGivesTheSameAnswers) {
    for (const std::size_t chunk : {1u, 2u, 7u, 64u}) {
        SCOPED_TRACE(chunk);
        Fixture f;
        QueueReader input;
        CollectingWriter output;
        neomcp::StdioOptions options = quiet();
        options.read_chunk_bytes = chunk;
        Serving run(f.server, input, output, options);
        input.push(line_of(ping(1)) + line_of(ping(2)));
        input.end();
        EXPECT_EQ(run.result(), 0);
        EXPECT_EQ(output.messages(), (std::vector<json>{pong(1), pong(2)}));
    }
}

TEST(Framing, BytesThatAreNotUtf8AreAParseErrorAndTheConnectionContinues) {
    Fixture f;
    QueueReader input;
    CollectingWriter output;
    Serving run(f.server, input, output, quiet());
    input.push("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\",\"x\":\"\xff\xfe\xc0\"}\n" + line_of(ping(2)));
    input.end();
    EXPECT_EQ(run.result(), 0);
    const auto messages = output.messages();
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(code_of(messages[0]), -32700);
    EXPECT_TRUE(messages[0].at("id").is_null());
    EXPECT_EQ(messages[1], pong(2));
}

TEST(Framing, ALineOverTheBoundIsAnsweredWithInvalidRequestAndTheNextLineIsServed) {
    neomcp::ServerOptions options = named("bounded");
    options.max_message_bytes = 100;
    neomcp::Server server(std::move(options));
    QueueReader input;
    CollectingWriter output;
    Serving run(server, input, output, quiet());

    // A request of exactly 100 bytes, with a carriage return: served. One byte more: refused.
    const auto padded = [](std::size_t size) {
        json request = ping(1);
        request["pad"] = "";
        const std::size_t base = request.dump().size();
        request["pad"] = std::string(size - base, 'p');
        return request.dump();
    };
    ASSERT_EQ(padded(100).size(), 100u);
    input.push(padded(100) + "\r\n");
    // Several times the bound, in several chunks.
    input.push(std::string(250, 'a'));
    input.push(std::string(250, 'b') + "\n");
    input.push(padded(101) + "\n");
    input.push(line_of(ping(2)));
    input.end();
    EXPECT_EQ(run.result(), 0);

    const auto messages = output.messages();
    ASSERT_EQ(messages.size(), 4u);
    EXPECT_EQ(messages[0], pong(1));
    EXPECT_EQ(code_of(messages[1]), -32600);
    EXPECT_TRUE(messages[1].at("id").is_null());
    EXPECT_EQ(code_of(messages[2]), -32600);
    EXPECT_EQ(messages[3], pong(2));
}

TEST(Framing, ALegacyHandshakeThenACall) {
    Fixture f;
    QueueReader input;
    CollectingWriter output;
    Serving run(f.server, input, output, quiet());
    input.push(line_of(legacy_initialize(1)));
    input.push(line_of(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}));
    input.push(line_of(legacy_call(2, "hello")));
    ASSERT_TRUE(output.wait_for_lines(2));
    input.end();
    EXPECT_EQ(run.result(), 0);
    const auto messages = output.messages();
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[0].at("id"), 1);
    EXPECT_EQ(messages[0].at("result").at("protocolVersion"), "2025-11-25");
    EXPECT_EQ(messages[1].at("id"), 2);
    EXPECT_EQ(text_of(messages[1]), "hello");
}

TEST(Framing, AModernRequestNeedsNoHandshake) {
    Fixture f;
    QueueReader input;
    CollectingWriter output;
    Serving run(f.server, input, output, quiet());
    input.push(line_of(modern_call(7, "hello")));
    input.end();
    EXPECT_EQ(run.result(), 0);
    const auto messages = output.messages();
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].at("id"), 7);
    EXPECT_EQ(text_of(messages[0]), "hello");
    EXPECT_EQ(messages[0].at("result").at("resultType"), "complete");
}

// ---------------------------------------------------------------------------------------------------------------
// The end of input

TEST(EndOfInput, ACallThatFinishesWithinTheDrainIsAnswered) {
    Fixture f;
    QueueReader input;
    CollectingWriter output;
    neomcp::StdioOptions options = quiet();
    options.drain_timeout = 120s;  // never reached by a working build: the answer ends the wait
    Serving run(f.server, input, output, options);
    input.push(line_of(modern_call(1, "gated")));
    ASSERT_TRUE(f.entered.wait(patience));
    input.end();
    // The input has ended while the call runs; the transport must still be waiting for it.
    f.gate.set();
    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(run.result(), 0);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 60s) << "the answer did not end the wait";
    const auto messages = output.messages();
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(text_of(messages[0]), "released");
}

TEST(EndOfInput, ACallThatOutlastsTheDrainSeesItsStopToken) {
    for (const auto drain : {0ms, 20ms}) {
        SCOPED_TRACE(drain.count());
        Fixture f;
        QueueReader input;
        CollectingWriter output;
        neomcp::StdioOptions options = quiet();
        options.drain_timeout = drain;
        Serving run(f.server, input, output, options);
        input.push(line_of(modern_call(1, "gated")));
        ASSERT_TRUE(f.entered.wait(patience));
        input.end();
        EXPECT_EQ(run.result(), 0);        // returned although the call is not answered
        EXPECT_TRUE(f.stopped.wait(patience));  // and the call was told to stop
        EXPECT_TRUE(output.messages().empty());
    }
}

TEST(EndOfInput, NothingInFlightReturnsAtOnce) {
    Fixture f;
    QueueReader input;
    CollectingWriter output;
    neomcp::StdioOptions options = quiet();
    options.drain_timeout = 120s;
    Serving run(f.server, input, output, options);
    input.push(line_of(ping(1)));
    ASSERT_TRUE(output.wait_for_lines(1));
    input.end();
    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(run.result(), 0);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 60s) << "an idle session was waited for";
}

TEST(EndOfInput, ACancelledCallDoesNotHoldTheDrain) {
    Fixture f;
    QueueReader input;
    CollectingWriter output;
    neomcp::StdioOptions options = quiet();
    options.drain_timeout = 120s;
    Serving run(f.server, input, output, options);
    input.push(line_of(modern_call(1, "gated")));
    ASSERT_TRUE(f.entered.wait(patience));
    input.push(line_of(json{{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", {{"requestId", 1}}}}));
    input.end();
    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(run.result(), 0);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 60s);
    EXPECT_TRUE(f.stopped.wait(patience));
    EXPECT_TRUE(output.messages().empty());
}

// ---------------------------------------------------------------------------------------------------------------
// A failed write

TEST(WriteFailure, OnTheReadingThreadEndsTheTransport) {
    Fixture f;
    QueueReader input;
    CollectingWriter output;
    output.fail_from(2);
    Serving run(f.server, input, output, quiet());
    input.push(line_of(ping(1)) + line_of(ping(2)) + line_of(ping(3)));
    // The input is still open: the transport ends because the write failed.
    EXPECT_EQ(run.result(), 0);
    EXPECT_EQ(output.messages(), (std::vector<json>{pong(1)}));
}

TEST(WriteFailure, OnAToolThreadInterruptsTheBlockedRead) {
    Fixture f;
    QueueReader input;
    CollectingWriter output;
    Serving run(f.server, input, output, quiet());
    input.push(line_of(modern_call(1, "gated")));
    ASSERT_TRUE(f.entered.wait(patience));
    output.fail_now();
    f.gate.set();  // the call completes on a worker thread, whose write fails
    // The reader is blocked in read() and the input never ends.
    EXPECT_EQ(run.result(), 0);
    EXPECT_TRUE(input.interrupted());
}

TEST(WriteFailure, ADiagnosticGoesToStandardErrorOnceAndOnlyWhenAsked) {
    for (const bool diagnostics : {true, false}) {
        SCOPED_TRACE(diagnostics);
        Fixture f;
        QueueReader input;
        CollectingWriter output;
        output.fail_now();
        neomcp::StdioOptions options;
        options.diagnostics = diagnostics;
        ::testing::internal::CaptureStderr();
        {
            Serving run(f.server, input, output, options);
            input.push(line_of(ping(1)) + line_of(ping(2)));
            EXPECT_EQ(run.result(), 0);
        }
        const std::string text = ::testing::internal::GetCapturedStderr();
        if (diagnostics) {
            EXPECT_NE(text.find("writing to the output stream failed"), std::string::npos) << text;
            EXPECT_EQ(text.find("writing to the output stream failed"), text.rfind("writing to the output stream failed"));
        } else {
            EXPECT_TRUE(text.empty()) << text;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Options

TEST(Options, ASessionThatCannotBeCreatedReturnsOneAndSaysWhy) {
    neomcp::ServerOptions options = named("twice");
    options.profiles = {builtin("2025-06-18"), builtin("2025-06-18")};
    neomcp::Server server(std::move(options));
    for (const bool diagnostics : {true, false}) {
        QueueReader input;
        CollectingWriter output;
        neomcp::StdioOptions stdio_options;
        stdio_options.diagnostics = diagnostics;
        ::testing::internal::CaptureStderr();
        const int code = neomcp::serve_streams(server, input, output, stdio_options);
        const std::string text = ::testing::internal::GetCapturedStderr();
        EXPECT_EQ(code, 1);
        EXPECT_TRUE(output.text().empty());
        if (diagnostics) {
            EXPECT_NE(text.find("cannot serve a session"), std::string::npos) << text;
            EXPECT_NE(text.find("2025-06-18"), std::string::npos) << text;
        } else {
            EXPECT_TRUE(text.empty()) << text;
        }
    }
}

TEST(Options, AZeroReadChunkIsRefused) {
    Fixture f;
    QueueReader input;
    CollectingWriter output;
    neomcp::StdioOptions options = quiet();
    options.read_chunk_bytes = 0;
    EXPECT_THROW(neomcp::serve_streams(f.server, input, output, options), std::invalid_argument);
}

#ifndef _WIN32

// The disposition of SIGPIPE, as the writer sees it while the transport runs.
void on_sigpipe(int) {}

bool sigpipe_is_ignored() {
    struct sigaction current = {};
    ::sigaction(SIGPIPE, nullptr, &current);
    return current.sa_handler == SIG_IGN;
}

class SigpipeWatchingWriter final : public neomcp::ByteWriter {
public:
    bool write(std::string_view) override {
        ignored = sigpipe_is_ignored();
        return true;
    }
    bool ignored = false;
};

TEST(Options, SigpipeIsIgnoredWhileTheTransportRunsAndRestoredAfter) {
    for (const bool ignore : {true, false}) {
        SCOPED_TRACE(ignore);
        struct sigaction ours = {};
        ours.sa_handler = on_sigpipe;
        sigemptyset(&ours.sa_mask);
        struct sigaction before = {};
        ASSERT_EQ(::sigaction(SIGPIPE, &ours, &before), 0);

        Fixture f;
        QueueReader input;
        SigpipeWatchingWriter output;
        neomcp::StdioOptions options = quiet();
        options.ignore_sigpipe = ignore;
        input.push(line_of(ping(1)));
        input.end();
        EXPECT_EQ(neomcp::serve_streams(f.server, input, output, options), 0);
        EXPECT_EQ(output.ignored, ignore);

        struct sigaction after = {};
        ::sigaction(SIGPIPE, nullptr, &after);
        EXPECT_EQ(after.sa_handler, on_sigpipe);
        ::sigaction(SIGPIPE, &before, nullptr);
    }
}

#endif

// ---------------------------------------------------------------------------------------------------------------
// Real descriptors

#ifdef _WIN32

TEST(Descriptors, SkippedOnWindows) {
    GTEST_SKIP() << "These tests build pipes with pipe(), which Windows does not have in this form. The Windows "
                    "streams (the C runtime's _read and _write in binary mode) are compiled by the Windows jobs, and "
                    "the same transport code runs over the in-memory streams above.";
}

#else

struct Pipe {
    Pipe() {
        int ends[2];
        if (::pipe(ends) != 0) throw std::runtime_error("pipe() failed");
        read_end = ends[0];
        write_end = ends[1];
    }
    ~Pipe() {
        close_read();
        close_write();
    }
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
    void close_read() {
        if (read_end >= 0) ::close(read_end);
        read_end = -1;
    }
    void close_write() {
        if (write_end >= 0) ::close(write_end);
        write_end = -1;
    }
    int read_end = -1;
    int write_end = -1;
};

// Everything that can be read from a descriptor until its end.
std::string read_all(int fd) {
    std::string out;
    char buffer[4096];
    for (;;) {
        const ssize_t count = ::read(fd, buffer, sizeof buffer);
        if (count > 0) {
            out.append(buffer, static_cast<std::size_t>(count));
        } else if (count == 0 || errno != EINTR) {
            return out;
        }
    }
}

TEST(Descriptors, AReaderDeliversWhatIsWrittenAndThenTheEnd) {
    Pipe pipe;
    neomcp::detail::FdReader reader(pipe.read_end);
    ASSERT_EQ(::write(pipe.write_end, "abcdef", 6), 6);
    char buffer[4];
    EXPECT_EQ(reader.read(buffer, sizeof buffer), 4u);
    EXPECT_EQ(std::string(buffer, 4), "abcd");
    EXPECT_EQ(reader.read(buffer, sizeof buffer), 2u);
    pipe.close_write();
    EXPECT_EQ(reader.read(buffer, sizeof buffer), 0u);
}

// A descriptor in non-blocking mode answers a read with EAGAIN when nothing has arrived. That is a reason to wait for
// more, not the end of the input: the reader returns the bytes that come and returns 0 only at the real end.
void expect_nonblocking_input_is_waited_on(bool without_wake_pipe) {
    Pipe pipe;
    ASSERT_NE(::fcntl(pipe.read_end, F_SETFL, ::fcntl(pipe.read_end, F_GETFL) | O_NONBLOCK), -1);
    // A reader without its private pipe is what one is when the pipe could not be made.
    neomcp::detail::FdReader reader(pipe.read_end, !without_wake_pipe);
    std::promise<std::size_t> first;
    std::promise<std::size_t> second;
    std::thread reading([&] {
        char buffer[8];
        first.set_value(reader.read(buffer, sizeof buffer));
        second.set_value(reader.read(buffer, sizeof buffer));
    });
    auto first_result = first.get_future();
    auto second_result = second.get_future();
    // Nothing has been written: the read is waiting, it has not returned 0.
    EXPECT_EQ(first_result.wait_for(300ms), std::future_status::timeout);
    EXPECT_EQ(::write(pipe.write_end, "abc", 3), 3);
    EXPECT_EQ(first_result.wait_for(patience), std::future_status::ready);
    if (first_result.wait_for(0s) == std::future_status::ready) {
        EXPECT_EQ(first_result.get(), 3u);
    }
    EXPECT_EQ(second_result.wait_for(300ms), std::future_status::timeout);
    pipe.close_write();  // the real end of the input
    EXPECT_EQ(second_result.wait_for(patience), std::future_status::ready);
    if (second_result.wait_for(0s) == std::future_status::ready) {
        EXPECT_EQ(second_result.get(), 0u);
    }
    reading.join();
}

TEST(Descriptors, ANonBlockingDescriptorIsWaitedOnAndNotTakenForTheEndOfTheInput) {
    expect_nonblocking_input_is_waited_on(false);
}

TEST(Descriptors, ANonBlockingDescriptorIsWaitedOnWhenTheReaderHasNoPipeOfItsOwn) {
    expect_nonblocking_input_is_waited_on(true);
}

TEST(Descriptors, AnInterruptEndsABlockedRead) {
    Pipe pipe;
    neomcp::detail::FdReader reader(pipe.read_end);
    std::promise<std::size_t> returned;
    std::thread blocked([&] {
        char buffer[4];
        returned.set_value(reader.read(buffer, sizeof buffer));
    });
    reader.interrupt();  // before or after the read began to block: either way it ends
    auto result = returned.get_future();
    EXPECT_EQ(result.wait_for(patience), std::future_status::ready);
    if (result.wait_for(0s) == std::future_status::ready) {
        EXPECT_EQ(result.get(), 0u);
    } else {
        pipe.close_write();  // unblock the thread so that the test can end
    }
    blocked.join();
    // The interrupt is lasting: data that arrives later is not read.
    ASSERT_EQ(::write(pipe.write_end, "x", 1), 1);
    char buffer[4];
    EXPECT_EQ(reader.read(buffer, sizeof buffer), 0u);
}

TEST(Descriptors, AWriterWritesEverythingEvenWhenThePipeIsSmallerThanTheData) {
    Pipe pipe;
    const std::string data(1 << 20, 'w');
    std::string received;
    std::thread drain([&] { received = read_all(pipe.read_end); });
    neomcp::detail::FdWriter writer(pipe.write_end);
    EXPECT_TRUE(writer.write(data));
    pipe.close_write();
    drain.join();
    EXPECT_EQ(received, data);
}

TEST(Descriptors, AWriterToAClosedPipeFailsInsteadOfEndingTheProcess) {
    Pipe pipe;
    pipe.close_read();
    const neomcp::detail::SigpipeGuard guard(true);
    neomcp::detail::FdWriter writer(pipe.write_end);
    EXPECT_FALSE(writer.write("lost\n"));
}

TEST(Descriptors, OverlappingSigpipeGuardsRestoreTheDispositionWhenTheLastIsGone) {
    struct sigaction handler = {};
    handler.sa_handler = [](int) {};
    sigemptyset(&handler.sa_mask);
    struct sigaction original = {};
    ASSERT_EQ(::sigaction(SIGPIPE, &handler, &original), 0);
    const auto disposition = [] {
        struct sigaction current = {};
        ::sigaction(SIGPIPE, nullptr, &current);
        return current.sa_handler;
    };
    auto first = std::make_unique<neomcp::detail::SigpipeGuard>(true);
    auto second = std::make_unique<neomcp::detail::SigpipeGuard>(true);
    EXPECT_EQ(disposition(), SIG_IGN);
    first.reset();  // the transport of the second guard still runs: its writes must not end the process
    EXPECT_EQ(disposition(), SIG_IGN);
    second.reset();
    EXPECT_EQ(disposition(), handler.sa_handler);
    ::sigaction(SIGPIPE, &original, nullptr);
}

TEST(Descriptors, ServeOverTwoPipes) {
    Fixture f;
    Pipe to_server;
    Pipe from_server;
    neomcp::detail::FdReader input(to_server.read_end);
    neomcp::detail::FdWriter output(from_server.write_end);
    std::promise<int> done;
    std::thread server([&] { done.set_value(neomcp::serve_streams(f.server, input, output, quiet())); });

    // A client that writes its requests, closes its end and then reads every answer, as a shell pipe does.
    const std::string requests = line_of(legacy_initialize(1)) + line_of(legacy_call(2, "hello")) + line_of(ping(3));
    ASSERT_EQ(::write(to_server.write_end, requests.data(), requests.size()), static_cast<ssize_t>(requests.size()));
    to_server.close_write();
    auto result = done.get_future();
    ASSERT_EQ(result.wait_for(patience), std::future_status::ready);
    EXPECT_EQ(result.get(), 0);
    server.join();
    from_server.close_write();
    const std::string answers = read_all(from_server.read_end);

    std::vector<json> messages;
    std::size_t start = 0;
    for (std::size_t end = answers.find('\n'); end != std::string::npos; end = answers.find('\n', start)) {
        messages.push_back(json::parse(answers.substr(start, end - start)));
        start = end + 1;
    }
    ASSERT_EQ(messages.size(), 3u);
    // The call runs on a worker, so its answer may come after the one to the ping that followed it.
    std::map<int, json> by_id;
    for (const auto& message : messages) by_id[message.at("id").get<int>()] = message;
    ASSERT_EQ(by_id.size(), 3u);
    EXPECT_TRUE(by_id.at(1).contains("result"));
    EXPECT_EQ(text_of(by_id.at(2)), "hello");
    EXPECT_EQ(by_id.at(3), pong(3));
}

TEST(Descriptors, AReaderOfTheOutputThatIsGoneEndsTheTransportWhileTheInputIsOpen) {
    Fixture f;
    Pipe to_server;
    Pipe from_server;
    from_server.close_read();  // the client's reading end is gone; the client's writing end is still open
    neomcp::detail::FdReader input(to_server.read_end);
    neomcp::detail::FdWriter output(from_server.write_end);
    std::promise<int> done;
    std::thread server([&] { done.set_value(neomcp::serve_streams(f.server, input, output, quiet())); });
    const std::string request = line_of(ping(1));
    ASSERT_EQ(::write(to_server.write_end, request.data(), request.size()), static_cast<ssize_t>(request.size()));
    auto result = done.get_future();
    const bool returned = result.wait_for(patience) == std::future_status::ready;
    EXPECT_TRUE(returned);
    if (!returned) to_server.close_write();  // let the server thread end
    server.join();
    if (returned) {
        EXPECT_EQ(result.get(), 0);
    }
}

#endif

}  // namespace
