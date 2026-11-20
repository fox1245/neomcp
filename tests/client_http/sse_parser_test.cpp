// The reader of server-sent events against the rules of the WHATWG HTML standard's event stream interpretation.
#include <gtest/gtest.h>

#include <string>
#include <variant>
#include <vector>

#include "neomcp/http/sse_parser.h"

namespace {

using neomcp::http::SseEvent;
using neomcp::http::SseItem;
using neomcp::http::SseLastEventId;
using neomcp::http::SseParser;
using neomcp::http::SseRetry;

constexpr std::size_t bound = 1024;

// Feeds the pieces in order and returns every item.
std::vector<SseItem> read(const std::vector<std::string>& pieces, std::size_t limit = bound) {
    SseParser parser(limit);
    std::vector<SseItem> items;
    for (const std::string& piece : pieces) parser.feed(piece, [&](SseItem item) { items.push_back(std::move(item)); });
    parser.finish();
    return items;
}

std::vector<std::string> data_of(const std::vector<SseItem>& items) {
    std::vector<std::string> data;
    for (const SseItem& item : items) {
        if (const auto* event = std::get_if<SseEvent>(&item)) data.push_back(event->data);
    }
    return data;
}

// Every split of `text` into two pieces gives the same items as the whole.
void same_at_every_split(const std::string& text) {
    const auto whole = data_of(read({text}));
    for (std::size_t split = 0; split <= text.size(); ++split) {
        EXPECT_EQ(data_of(read({text.substr(0, split), text.substr(split)})), whole) << "split at " << split;
    }
}

TEST(SseParser, OneEventPerBlankLine) {
    EXPECT_EQ(data_of(read({"data: one\n\ndata: two\n\n"})), (std::vector<std::string>{"one", "two"}));
}

TEST(SseParser, EveryLineEndEndsALine) {
    EXPECT_EQ(data_of(read({"data: lf\n\n"})), std::vector<std::string>{"lf"});
    EXPECT_EQ(data_of(read({"data: cr\r\r"})), std::vector<std::string>{"cr"});
    EXPECT_EQ(data_of(read({"data: crlf\r\n\r\n"})), std::vector<std::string>{"crlf"});
    EXPECT_EQ(data_of(read({"data: a\r\ndata: b\rdata: c\n\n"})), std::vector<std::string>{"a\nb\nc"});
}

TEST(SseParser, ACarriageReturnAtTheEndOfAPieceAndALineFeedAtTheStartOfTheNextAreOneLineEnd) {
    // Were they two line ends, the empty line between them would dispatch "a" alone.
    EXPECT_EQ(data_of(read({"data: a\r", "\ndata: b\r\n\r\n"})), std::vector<std::string>{"a\nb"});
    same_at_every_split("data: a\r\ndata: b\r\n\r\ndata: c\r\r");
}

TEST(SseParser, AByteOrderMarkIsDroppedOnlyAtTheStartEvenWhenSplit) {
    const std::string mark = "\xEF\xBB\xBF";
    EXPECT_EQ(data_of(read({mark + "data: x\n\n"})), std::vector<std::string>{"x"});
    EXPECT_EQ(data_of(read({"\xEF", "\xBB", "\xBF" "data: x\n\n"})), std::vector<std::string>{"x"});
    // Later, the same bytes are content.
    EXPECT_EQ(data_of(read({"data: x\n\ndata: " + mark + "y\n\n"})), (std::vector<std::string>{"x", mark + "y"}));
    // Only one mark is dropped: a second one makes the first line's field name differ from `data`.
    EXPECT_TRUE(data_of(read({mark + mark + "data: x\n\n"})).empty());
    // Bytes that start like a mark and are not one are kept.
    EXPECT_TRUE(data_of(read({"\xEF" "data: x\n\n"})).empty());
}

TEST(SseParser, DataLinesAreJoinedWithLineFeeds) {
    EXPECT_EQ(data_of(read({"data: first\ndata: second\ndata\ndata: third\n\n"})),
              std::vector<std::string>{"first\nsecond\n\nthird"});
}

TEST(SseParser, OnlyOneSpaceAfterTheColonIsDropped) {
    EXPECT_EQ(data_of(read({"data:  two spaces\n\n"})), std::vector<std::string>{" two spaces"});
    EXPECT_EQ(data_of(read({"data:none\n\n"})), std::vector<std::string>{"none"});
    EXPECT_EQ(data_of(read({"data:\tkept tab\n\n"})), std::vector<std::string>{"\tkept tab"});
}

TEST(SseParser, ALineWithoutAColonIsAFieldWithAnEmptyValue) {
    // `data` alone appends an empty line: two of them make the data "\n".
    EXPECT_EQ(data_of(read({"data\ndata\n\n"})), std::vector<std::string>{"\n"});
}

TEST(SseParser, CommentsAndUnknownFieldsAreIgnored) {
    EXPECT_EQ(data_of(read({": keep-alive\n: another\nunknown: x\ndata: real\n\n"})), std::vector<std::string>{"real"});
    // A block of comments only dispatches nothing.
    EXPECT_TRUE(read({": one\n\n: two\n\n"}).empty());
}

TEST(SseParser, AnEventTypeIsTheEventFieldOrMessage) {
    const auto items = read({"event: endpoint\ndata: /messages\n\ndata: plain\n\nevent:\ndata: empty type\n\n"});
    std::vector<std::string> types;
    for (const SseItem& item : items) {
        if (const auto* event = std::get_if<SseEvent>(&item)) types.push_back(event->type);
    }
    EXPECT_EQ(types, (std::vector<std::string>{"endpoint", "message", "message"}));
}

TEST(SseParser, AnEventWithEmptyDataIsNotDispatchedButItsIdCounts) {
    // The priming event of a stream: an id and a data line with nothing in it.
    const auto items = read({"id: event-1\ndata:\n\n"});
    ASSERT_EQ(items.size(), 2u);
    ASSERT_TRUE(std::holds_alternative<SseLastEventId>(items[0]));
    EXPECT_EQ(std::get<SseLastEventId>(items[0]).id, "event-1");
    // `data:` with nothing appends an empty line, so the data is the empty string and the event is dispatched.
    ASSERT_TRUE(std::holds_alternative<SseEvent>(items[1]));
    EXPECT_EQ(std::get<SseEvent>(items[1]).data, "");
    // Without any data line no event is dispatched, but the id still changes.
    const auto id_only = read({"id: event-2\n\n"});
    ASSERT_EQ(id_only.size(), 1u);
    EXPECT_EQ(std::get<SseLastEventId>(id_only[0]).id, "event-2");
}

TEST(SseParser, AnIdWithNulIsIgnored) {
    EXPECT_TRUE(read({std::string("id: a\0b\n\n", 9)}).empty());
    const auto items = read({std::string("id: kept\nid: a\0b\ndata: x\n\n", 26)});
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(std::get<SseLastEventId>(items[0]).id, "kept");
}

TEST(SseParser, AnEmptyIdClearsTheLastId) {
    const auto items = read({"id\n\n"});
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(std::get<SseLastEventId>(items[0]).id, "");
}

TEST(SseParser, RetryTakesAsciiDigitsOnly) {
    const auto items = read({"retry: 500\nretry: 5x\nretry: -1\nretry: 1.5\nretry: \nretry:  7\nretry\n"
                             "retry: 99999999999999999999999999\nretry: 0\n\n"});
    std::vector<long long> delays;
    for (const SseItem& item : items) {
        if (const auto* retry = std::get_if<SseRetry>(&item)) delays.push_back(retry->delay.count());
    }
    EXPECT_EQ(delays, (std::vector<long long>{500, 0}));
}

TEST(SseParser, WhatIsPendingAtTheEndIsDiscarded) {
    EXPECT_TRUE(data_of(read({"data: no blank line\n"})).empty());
    EXPECT_TRUE(data_of(read({"data: no line end"})).empty());
    EXPECT_EQ(data_of(read({"data: done\n\ndata: pending\n"})), std::vector<std::string>{"done"});
}

TEST(SseParser, AnEventOverTheBoundStopsTheStream) {
    SseParser parser(16);
    std::vector<SseItem> items;
    const auto sink = [&](SseItem item) { items.push_back(std::move(item)); };
    EXPECT_EQ(parser.feed("data: 0123456789\n\n", sink), SseParser::Status::reading);
    EXPECT_EQ(parser.feed("data: 0123456789\ndata: 0123456789\n\n", sink), SseParser::Status::too_large);
    EXPECT_GT(parser.take_oversized().size(), 16u);
    EXPECT_EQ(parser.feed("data: more\n\n", sink), SseParser::Status::too_large);
    EXPECT_EQ(parser.finish(), SseParser::Status::too_large);
    EXPECT_EQ(data_of(items), std::vector<std::string>{"0123456789"});
}

TEST(SseParser, ALineThatNeverEndsIsBoundedToo) {
    SseParser parser(64);
    const auto sink = [](SseItem) {};
    // An endless comment: no event ever comes, and the pending line must not grow beyond the bound.
    SseParser::Status status = SseParser::Status::reading;
    for (int i = 0; i < 100 && status == SseParser::Status::reading; ++i) status = parser.feed(": ........", sink);
    EXPECT_EQ(status, SseParser::Status::too_large);
    const std::size_t held = parser.take_oversized().size();
    EXPECT_GT(held, 64u);
    EXPECT_LE(held, 64u + SseParser::line_overhead + 1);
}

TEST(SseParser, AnEventAtTheBoundIsRead) {
    const std::string data(16, 'x');
    EXPECT_EQ(data_of(read({"data: " + data + "\n\n"}, 16)), std::vector<std::string>{data});
    EXPECT_EQ(data_of(read({"data: " + data.substr(0, 8) + "\ndata: " + data.substr(0, 7) + "\n\n"}, 16)),
              std::vector<std::string>{data.substr(0, 8) + "\n" + data.substr(0, 7)});
    EXPECT_TRUE(data_of(read({"data: " + data + "x\n\n"}, 16)).empty());
}

TEST(SseParser, ABoundOfZeroIsRefused) { EXPECT_THROW(SseParser(0), std::invalid_argument); }

TEST(SseParser, PiecesOfOneByteGiveTheSameEvents) {
    const std::string text = "\xEF\xBB\xBF" "id: 1\r\nevent: message\r\ndata: {\"a\":1}\r\n\r\n: c\rdata: b\n\n";
    std::vector<std::string> pieces;
    for (const char c : text) pieces.emplace_back(1, c);
    EXPECT_EQ(data_of(read(pieces)), data_of(read({text})));
    EXPECT_EQ(data_of(read({text})), (std::vector<std::string>{"{\"a\":1}", "b"}));
}

}  // namespace
