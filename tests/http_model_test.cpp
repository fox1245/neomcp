// Tests of the HTTP model: header fields, the responder, the body stream and server-sent events framing.
//
// Every wait is on an event a test controls (a promise, a latch); the bound on a wait only limits how long a broken
// build takes to fail.
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/http/body_stream.h"
#include "neomcp/http/media_type.h"
#include "neomcp/http/message.h"
#include "neomcp/http/sse.h"

namespace {

using namespace std::chrono_literals;
using neomcp::http::BodyStream;
using neomcp::http::Headers;
using neomcp::http::Responder;
using neomcp::http::Response;

// Long enough for a loaded machine; reached only when something is broken.
constexpr std::chrono::milliseconds patience = 30s;

// ---- Headers ---------------------------------------------------------------------------------------------------------

TEST(HttpHeaders, LookupIgnoresCaseAndKeepsOrder) {
    Headers headers;
    headers.add("Content-Type", "application/json");
    headers.add("X-One", "1");
    headers.add("x-one", "2");
    EXPECT_EQ(headers.get("content-type"), "application/json");
    EXPECT_EQ(headers.get("CONTENT-TYPE"), "application/json");
    EXPECT_EQ(headers.get("X-ONE"), "1");  // the first of two
    EXPECT_EQ(headers.count("X-One"), 2u);
    const std::vector<std::string_view> all = headers.get_all("x-ONE");
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0], "1");
    EXPECT_EQ(all[1], "2");
    ASSERT_EQ(headers.size(), 3u);
    EXPECT_EQ(headers.fields()[0].name, "Content-Type");
    EXPECT_EQ(headers.fields()[1].name, "X-One");
    EXPECT_EQ(headers.fields()[2].name, "x-one");
    EXPECT_FALSE(headers.get("absent").has_value());
    EXPECT_FALSE(headers.contains("absent"));
}

TEST(HttpHeaders, SetReplacesEveryFieldOfThatNameAtTheFirstPosition) {
    Headers headers{{"A", "1"}, {"B", "2"}, {"a", "3"}};
    headers.set("A", "9");
    ASSERT_EQ(headers.size(), 2u);
    EXPECT_EQ(headers.fields()[0].name, "A");
    EXPECT_EQ(headers.fields()[0].value, "9");
    EXPECT_EQ(headers.fields()[1].name, "B");
    headers.set("C", "4");
    ASSERT_EQ(headers.size(), 3u);
    EXPECT_EQ(headers.fields()[2].name, "C");
}

TEST(HttpHeaders, RemoveReportsHowManyWereRemoved) {
    Headers headers{{"A", "1"}, {"B", "2"}, {"a", "3"}};
    EXPECT_EQ(headers.remove("A"), 2u);
    EXPECT_EQ(headers.remove("A"), 0u);
    ASSERT_EQ(headers.size(), 1u);
    EXPECT_EQ(headers.fields()[0].name, "B");
}

TEST(HttpHeaders, EqualityFoldsOnlyAsciiLetters) {
    EXPECT_TRUE(neomcp::http::equal_ignoring_case("Host", "hOST"));
    EXPECT_FALSE(neomcp::http::equal_ignoring_case("Host", "Hosts"));
    EXPECT_FALSE(neomcp::http::equal_ignoring_case("a-b", "a_b"));
    EXPECT_TRUE(neomcp::http::equal_ignoring_case("", ""));
}

// ---- Responder -------------------------------------------------------------------------------------------------------

struct Collector {
    std::vector<int> statuses;
    std::atomic<int> calls{0};
    bool accept = true;
    Responder::Sink sink() {
        return [this](Response&& response) {
            statuses.push_back(response.status);
            ++calls;
            return accept;
        };
    }
};

TEST(HttpResponder, CompletesOnce) {
    Collector collector;
    {
        Responder responder(collector.sink());
        EXPECT_FALSE(responder.completed());
        EXPECT_TRUE(responder.complete(neomcp::http::make_response(200)));
        EXPECT_TRUE(responder.completed());
        EXPECT_FALSE(responder.complete(neomcp::http::make_response(201)));
    }
    ASSERT_EQ(collector.calls, 1);
    EXPECT_EQ(collector.statuses[0], 200);
}

TEST(HttpResponder, AnUncompletedResponderAnswers500WhenItsLastCopyIsDestroyed) {
    Collector collector;
    {
        Responder responder(collector.sink());
        Responder copy = responder;
        responder = Responder([](Response&&) { return true; });  // the first copy goes away
        EXPECT_EQ(collector.calls, 0);                            // the other one is still alive
    }
    ASSERT_EQ(collector.calls, 1);
    EXPECT_EQ(collector.statuses[0], 500);
}

TEST(HttpResponder, ACompletedResponderIsNotAnswered500Later) {
    Collector collector;
    {
        Responder responder(collector.sink());
        responder.complete(neomcp::http::make_response(204));
    }
    EXPECT_EQ(collector.calls, 1);  // completing suppressed the 500
}

TEST(HttpResponder, CopiesShareOneCompletion) {
    Collector collector;
    Responder first(collector.sink());
    Responder second = first;
    EXPECT_TRUE(second.complete(neomcp::http::make_response(202)));
    EXPECT_TRUE(first.completed());
    EXPECT_FALSE(first.complete(neomcp::http::make_response(500)));
    EXPECT_EQ(collector.calls, 1);
}

TEST(HttpResponder, ReportsWhenTheListenerRefusedTheResponse) {
    Collector collector;
    collector.accept = false;
    Responder responder(collector.sink());
    EXPECT_FALSE(responder.complete(neomcp::http::make_response(200)));
    EXPECT_EQ(collector.calls, 1);
}

TEST(HttpResponder, CompletesLaterFromAnotherThread) {
    std::promise<int> status;
    auto answered = status.get_future();
    Responder responder([&](Response&& response) {
        status.set_value(response.status);
        return true;
    });
    std::thread other([responder] { responder.complete(neomcp::http::make_response(201)); });
    ASSERT_EQ(answered.wait_for(patience), std::future_status::ready);
    EXPECT_EQ(answered.get(), 201);
    other.join();
}

TEST(HttpResponder, ASinkThatThrowsDoesNotEscape) {
    Responder responder([](Response&&) -> bool { throw 1; });
    EXPECT_FALSE(responder.complete(neomcp::http::make_response(200)));
}

TEST(HttpResponder, AReportedDisconnectRunsTheCallbackOnceAndDropsALaterCompletion) {
    Collector collector;
    Responder responder(collector.sink());
    int told = 0;
    responder.on_disconnect([&] { ++told; });
    const Responder::Watch watch = responder.watch();
    EXPECT_TRUE(watch.report_disconnect());
    EXPECT_EQ(told, 1);
    EXPECT_TRUE(responder.completed());
    EXPECT_FALSE(watch.report_disconnect());  // reported once
    EXPECT_EQ(told, 1);
    EXPECT_FALSE(responder.complete(neomcp::http::make_response(200)));
    EXPECT_EQ(collector.calls, 0);  // the listener never gets the late answer
}

TEST(HttpResponder, ADisconnectAfterTheCompletionIsNotReported) {
    Collector collector;
    Responder responder(collector.sink());
    int told = 0;
    responder.on_disconnect([&] { ++told; });
    EXPECT_TRUE(responder.complete(neomcp::http::make_response(200)));
    EXPECT_FALSE(responder.watch().report_disconnect());
    EXPECT_EQ(told, 0);
}

TEST(HttpResponder, ARegistrationAfterTheDisconnectRunsAtOnce) {
    Collector collector;
    Responder responder(collector.sink());
    ASSERT_TRUE(responder.watch().report_disconnect());
    int told = 0;
    responder.on_disconnect([&] { ++told; });
    EXPECT_EQ(told, 1);
}

TEST(HttpResponder, AWatchDoesNotKeepTheRequestFromBeingAnswered500) {
    Collector collector;
    Responder::Watch watch;
    {
        Responder responder(collector.sink());
        watch = responder.watch();
    }
    ASSERT_EQ(collector.calls, 1);
    EXPECT_EQ(collector.statuses[0], 500);
    EXPECT_FALSE(watch.report_disconnect());  // the request is gone
}

TEST(HttpResponder, ADisconnectedRequestIsNotAnswered500WhenItsLastCopyGoes) {
    Collector collector;
    {
        Responder responder(collector.sink());
        ASSERT_TRUE(responder.watch().report_disconnect());
    }
    EXPECT_EQ(collector.calls, 0);
}

TEST(HttpResponder, ACallbackThatThrowsDoesNotEscapeTheReport) {
    Collector collector;
    Responder responder(collector.sink());
    responder.on_disconnect([] { throw 1; });
    EXPECT_TRUE(responder.watch().report_disconnect());
}

TEST(HttpResponder, ACompletionRacingADisconnectSettlesTheRequestExactlyOnce) {
    for (int round = 0; round < 200; ++round) {
        std::atomic<int> delivered{0};
        std::atomic<int> told{0};
        Responder responder([&](Response&&) {
            ++delivered;
            return true;
        });
        responder.on_disconnect([&] { ++told; });
        const Responder::Watch watch = responder.watch();
        std::atomic<bool> completed{false};
        std::thread completer([&] { completed = responder.complete(neomcp::http::make_response(200)); });
        const bool reported = watch.report_disconnect();
        completer.join();
        EXPECT_NE(reported, completed.load());
        EXPECT_EQ(delivered.load() + told.load(), 1);
    }
}

TEST(HttpMessage, MakeResponseSetsContentType) {
    const Response response = neomcp::http::make_response(200, "application/json", "{}");
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.headers.get("content-type"), "application/json");
    EXPECT_EQ(response.body, "{}");
    EXPECT_FALSE(response.stream);
    EXPECT_FALSE(neomcp::http::make_response(204).headers.contains("Content-Type"));
}

// ---- BodyStream ------------------------------------------------------------------------------------------------------

TEST(HttpBodyStream, DeliversChunksInOrderAndEnds) {
    BodyStream stream;
    EXPECT_EQ(stream.append("one"), BodyStream::Append::accepted);
    EXPECT_EQ(stream.append("two"), BodyStream::Append::accepted);
    stream.end();
    std::string out;
    EXPECT_EQ(stream.read(out, 1s), BodyStream::Read::data);
    EXPECT_EQ(out, "onetwo");
    EXPECT_EQ(stream.read(out, 1s), BodyStream::Read::end);
    EXPECT_EQ(stream.read(out, 1s), BodyStream::Read::end);
}

TEST(HttpBodyStream, ReadTimesOutWhenNothingArrives) {
    BodyStream stream;
    std::string out;
    EXPECT_EQ(stream.read(out, 10ms), BodyStream::Read::timeout);
    EXPECT_FALSE(stream.closed());
}

TEST(HttpBodyStream, AReadWaitingForDataWakesWhenAChunkIsAppended) {
    BodyStream stream;
    std::promise<std::string> got;
    auto result = got.get_future();
    std::thread consumer([&] {
        std::string out;
        if (stream.read(out, patience) == BodyStream::Read::data) got.set_value(out);
        else got.set_value("<no data>");
    });
    stream.append("hello");
    ASSERT_EQ(result.wait_for(patience), std::future_status::ready);
    EXPECT_EQ(result.get(), "hello");
    consumer.join();
}

TEST(HttpBodyStream, AReadWaitingForDataWakesWhenTheProducerEnds) {
    BodyStream stream;
    std::promise<BodyStream::Read> got;
    auto result = got.get_future();
    std::thread consumer([&] {
        std::string out;
        got.set_value(stream.read(out, patience));
    });
    stream.end();
    ASSERT_EQ(result.wait_for(patience), std::future_status::ready);
    EXPECT_EQ(result.get(), BodyStream::Read::end);
    consumer.join();
}

TEST(HttpBodyStream, AppendBeyondTheBoundFailsAndChangesNothing) {
    BodyStream stream(8);
    EXPECT_EQ(stream.append("12345"), BodyStream::Append::accepted);
    EXPECT_EQ(stream.append("6789"), BodyStream::Append::full);  // 9 > 8
    EXPECT_EQ(stream.append("678"), BodyStream::Append::accepted);  // exactly 8
    EXPECT_EQ(stream.append("x"), BodyStream::Append::full);
    EXPECT_FALSE(stream.closed());
    std::string out;
    EXPECT_EQ(stream.read(out, 1s), BodyStream::Read::data);
    EXPECT_EQ(out, "12345678");
    // The bound counts unread bytes: after a read there is room again.
    EXPECT_EQ(stream.append("abcdefgh"), BodyStream::Append::accepted);
    EXPECT_EQ(stream.append("abcdefghi"), BodyStream::Append::full);
}

TEST(HttpBodyStream, ACloseMakesAppendsNoOpsAndDiscardsTheBuffer) {
    BodyStream stream;
    stream.append("pending");
    stream.close();
    EXPECT_TRUE(stream.closed());
    EXPECT_EQ(stream.append("more"), BodyStream::Append::closed);
    std::string out;
    EXPECT_EQ(stream.read(out, 1s), BodyStream::Read::closed);
    EXPECT_TRUE(out.empty());
}

TEST(HttpBodyStream, AppendAfterTheEndIsRefused) {
    BodyStream stream;
    stream.end();
    EXPECT_EQ(stream.append("late"), BodyStream::Append::ended);
}

TEST(HttpBodyStream, TheCloseCallbackRunsOnceAndOutsideTheLock) {
    BodyStream stream;
    std::atomic<int> calls{0};
    std::atomic<bool> reentered{false};
    stream.on_close([&] {
        ++calls;
        // Calling back into the stream would deadlock if the callback ran under its lock.
        reentered = stream.closed() && stream.append("x") == BodyStream::Append::closed;
    });
    stream.close();
    stream.close();
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(reentered);
}

TEST(HttpBodyStream, ARegistrationOnAClosedStreamRunsAtOnce) {
    BodyStream stream;
    stream.close();
    int calls = 0;
    stream.on_close([&] { ++calls; });
    EXPECT_EQ(calls, 1);
}

TEST(HttpBodyStream, ClosingAFinishedStreamIsNotAClose) {
    BodyStream stream;
    int calls = 0;
    stream.on_close([&] { ++calls; });
    stream.append("done");
    stream.end();
    std::string out;
    ASSERT_EQ(stream.read(out, 1s), BodyStream::Read::data);
    ASSERT_EQ(stream.read(out, 1s), BodyStream::Read::end);
    stream.close();
    EXPECT_EQ(calls, 0);
    EXPECT_FALSE(stream.closed());
}

TEST(HttpBodyStream, ClosingAnEndedButUnreadStreamIsAClose) {
    BodyStream stream;
    int calls = 0;
    stream.on_close([&] { ++calls; });
    stream.append("unread");
    stream.end();
    stream.close();
    EXPECT_EQ(calls, 1);
}

TEST(HttpBodyStream, AReadWaitingForDataWakesWhenTheConsumerCloses) {
    BodyStream stream;
    std::promise<BodyStream::Read> got;
    auto result = got.get_future();
    std::thread consumer([&] {
        std::string out;
        got.set_value(stream.read(out, patience));
    });
    stream.close();
    ASSERT_EQ(result.wait_for(patience), std::future_status::ready);
    EXPECT_EQ(result.get(), BodyStream::Read::closed);
    consumer.join();
}

TEST(HttpBodyStream, ACallbackThatThrowsDoesNotEscapeClose) {
    BodyStream stream;
    stream.on_close([] { throw 1; });
    EXPECT_NO_THROW(stream.close());
}

TEST(HttpBodyStream, ManyProducersAndOneConsumerLoseNothing) {
    BodyStream stream(std::size_t{1} << 24);
    constexpr int producers = 4;
    constexpr int per_producer = 500;
    std::vector<std::thread> threads;
    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&] {
            for (int i = 0; i < per_producer; ++i) {
                while (stream.append("x") == BodyStream::Append::full) std::this_thread::yield();
            }
        });
    }
    std::size_t total = 0;
    std::thread consumer([&] {
        std::string out;
        while (total < static_cast<std::size_t>(producers) * per_producer) {
            if (stream.read(out, patience) != BodyStream::Read::data) break;
            total += out.size();
        }
    });
    for (auto& t : threads) t.join();
    consumer.join();
    EXPECT_EQ(total, static_cast<std::size_t>(producers) * per_producer);
}

// ---- Server-sent events framing --------------------------------------------------------------------------------------

TEST(HttpSse, ASingleLinePayloadIsOneDataLine) {
    EXPECT_EQ(neomcp::http::sse_event("{\"a\":1}"), "data: {\"a\":1}\n\n");
}

TEST(HttpSse, AMultiLinePayloadBecomesSeveralDataLines) {
    EXPECT_EQ(neomcp::http::sse_event("one\ntwo\nthree"), "data: one\ndata: two\ndata: three\n\n");
    EXPECT_EQ(neomcp::http::sse_event("one\r\ntwo"), "data: one\ndata: two\n\n");
    EXPECT_EQ(neomcp::http::sse_event("one\rtwo"), "data: one\ndata: two\n\n");  // a receiver splits at "\r" too
}

TEST(HttpSse, EmptyLinesAreKept) {
    EXPECT_EQ(neomcp::http::sse_event(""), "data:\n\n");
    EXPECT_EQ(neomcp::http::sse_event("a\n\nb"), "data: a\ndata:\ndata: b\n\n");
    EXPECT_EQ(neomcp::http::sse_event("a\n"), "data: a\ndata:\n\n");
}

TEST(HttpSse, ACommentIsOneLineAndABlankLine) {
    EXPECT_EQ(neomcp::http::sse_comment("ping"), ": ping\n\n");
    EXPECT_EQ(neomcp::http::sse_comment("a\nb\rc"), ": a b c\n\n");
    EXPECT_EQ(neomcp::http::sse_keep_alive().front(), ':');
    EXPECT_TRUE(neomcp::http::sse_keep_alive().ends_with("\n\n"));
}

TEST(HttpSse, EventStreamContentTypeIsRecognisedByItsMediaType) {
    using neomcp::http::is_event_stream_content_type;
    EXPECT_TRUE(is_event_stream_content_type("text/event-stream"));
    EXPECT_TRUE(is_event_stream_content_type("Text/Event-Stream"));
    EXPECT_TRUE(is_event_stream_content_type("  text/event-stream ; charset=utf-8"));
    EXPECT_TRUE(is_event_stream_content_type("text/event-stream;charset=utf-8"));
    EXPECT_FALSE(is_event_stream_content_type("text/event-stream2"));
    EXPECT_FALSE(is_event_stream_content_type("application/json; x=text/event-stream"));
    EXPECT_FALSE(is_event_stream_content_type(""));
}

// ---- The media type parser -------------------------------------------------------------------------------------------

TEST(HttpMediaType, ReadsTypeSubtypeAndParameters) {
    const auto media = neomcp::http::parse_media_type("Application/JSON ; charset=\"utf-8\";q=x");
    ASSERT_TRUE(media.has_value());
    EXPECT_EQ(media->type, "application");
    EXPECT_EQ(media->subtype, "json");
    ASSERT_EQ(media->parameters.size(), 2U);
    EXPECT_EQ(media->parameters[0].first, "charset");
    EXPECT_EQ(media->parameters[0].second, "utf-8");
    EXPECT_TRUE(neomcp::http::is_media_type("application/json;charset=utf-8", "application", "json"));
    EXPECT_FALSE(neomcp::http::is_media_type("application/jsonx", "application", "json"));
    EXPECT_FALSE(neomcp::http::is_media_type("text/json", "application", "json"));
}

TEST(HttpMediaType, RefusesWhatTheGrammarDoesNotAllow) {
    for (const char* bad : {"", "application", "application/", "/json", "application/json;", "application/json; x",
                            "application/json; x=", "application/json; x=\"open", "application/json garbage",
                            "appli cation/json", "application/json; x=a b"}) {
        if (std::string(bad) == "application/json;") {
            EXPECT_TRUE(neomcp::http::parse_media_type(bad).has_value()) << "an empty parameter is allowed";
            continue;
        }
        EXPECT_FALSE(neomcp::http::parse_media_type(bad).has_value()) << bad;
    }
}

TEST(HttpMediaType, AcceptRangesAndWeights) {
    using neomcp::http::accepts;
    const auto ranges = neomcp::http::parse_accept("text/event-stream, application/json;q=0.5,,*/*;q=0");
    ASSERT_TRUE(ranges.has_value());
    ASSERT_EQ(ranges->size(), 3U);
    EXPECT_EQ((*ranges)[1].weight, 500);
    EXPECT_TRUE(accepts(*ranges, "text", "event-stream"));
    EXPECT_TRUE(accepts(*ranges, "application", "json"));
    EXPECT_FALSE(accepts(*ranges, "text", "html")) << "only */* matches, and its weight is 0";

    const auto refused = neomcp::http::parse_accept("application/json;q=0, */*");
    ASSERT_TRUE(refused.has_value());
    EXPECT_FALSE(accepts(*refused, "application", "json")) << "the most specific range decides";
    EXPECT_TRUE(accepts(*refused, "text", "event-stream"));

    const auto any = neomcp::http::parse_accept("application/*");
    ASSERT_TRUE(any.has_value());
    EXPECT_TRUE(accepts(*any, "application", "json"));
    EXPECT_FALSE(accepts(*any, "text", "event-stream"));

    EXPECT_TRUE(neomcp::http::parse_accept("")->empty());
    for (const char* bad : {"*/json", "text", "text/html;q=2", "text/html;q=0.1234", "text/html;q=\"1\"",
                            "text/html;q=1;q=1", "text/html;q=1.001", "text/html text/plain"}) {
        EXPECT_FALSE(neomcp::http::parse_accept(bad).has_value()) << bad;
    }
}

}  // namespace
