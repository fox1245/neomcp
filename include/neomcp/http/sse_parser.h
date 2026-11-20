// Reading server-sent events (the text/event-stream format) as the WHATWG HTML standard's event stream
// interpretation defines it, incrementally: bytes go in as they arrive, in pieces of any size, and events come out.
//
// The rules (HTML, "Parsing an event stream" and "Interpreting an event stream"):
// - A line ends at CR, LF or CRLF; a CR that ends one piece and an LF that starts the next are one line end.
// - One byte order mark (EF BB BF) at the very start of the stream is dropped, also when its bytes arrive apart.
// - An empty line dispatches the event. A line that starts with ':' is a comment and is ignored. Otherwise the field
//   name is what comes before the first ':' (the whole line when there is none, with an empty value), and one space
//   after the ':' is dropped from the value.
// - `data` appends the value and an LF to the event's data; the last LF is dropped when the event is dispatched.
//   `event` sets the event's type. `id` sets the stream's last event id, unless the value contains NUL. `retry` sets
//   the reconnection time when the value is ASCII digits only. Other fields are ignored.
// - An event whose data is empty is not dispatched; its id still becomes the stream's last event id.
// - What is pending when the stream ends (an event without its blank line, a line without its end) is discarded.
//
// Bounds: one event's data (its lines joined) may not exceed `max_event_bytes`, and the line being read, together
// with the data held, may not exceed it by more than `line_overhead` bytes (room for a field name and its colon and
// space). A stream that exceeds either stops: the parser reports too_large, keeps what it held for the caller to see,
// and reads nothing more. So a line that never ends (an endless comment) is bounded as well.
//
// The parser knows nothing of a protocol carried in the events.
#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace neomcp::http {

// The type of an event that names none, and of one whose `event` field is empty.
inline constexpr std::string_view sse_default_event_type = "message";

// A dispatched event.
struct SseEvent {
    std::string type;  // sse_default_event_type unless an `event` field named another
    std::string data;  // the `data` lines joined with LF
};

// The stream's last event id changed: an `id` field was read in the event block that an empty line just ended
// (whether or not that block dispatched an event). The id may be empty, which clears it.
struct SseLastEventId {
    std::string id;
};

// A `retry` field with a valid value: how long to wait before reconnecting.
struct SseRetry {
    std::chrono::milliseconds delay{0};
};

using SseItem = std::variant<SseEvent, SseLastEventId, SseRetry>;

class SseParser {
public:
    enum class Status {
        reading,    // more may come
        too_large,  // the bound was exceeded; nothing more is read
        ended       // finish() was called
    };

    // The bytes a line may hold beyond the bound of the data: a field name, its colon and one space.
    static constexpr std::size_t line_overhead = 16;

    // A bound of zero is an error (std::invalid_argument).
    explicit SseParser(std::size_t max_event_bytes);

    // Reads a piece of the stream; every item it completes goes to `sink`, in order. Returns the status after the
    // piece. Once the status is not `reading`, further pieces are ignored.
    Status feed(std::string_view bytes, const std::function<void(SseItem)>& sink);

    // The stream ended: what is pending is discarded. Returns `ended`, or `too_large` when the bound was exceeded.
    Status finish();

    Status status() const noexcept { return status_; }

    // too_large: the event's data and the pending line as held when the bound was exceeded (longer than the bound).
    // Empty otherwise.
    std::string take_oversized();

private:
    void line(std::string_view text, const std::function<void(SseItem)>& sink);
    void dispatch(const std::function<void(SseItem)>& sink);
    std::size_t line_room() const noexcept;  // the bytes the pending line may still take

    std::size_t max_event_bytes_;
    Status status_ = Status::reading;
    std::size_t bom_seen_ = 0;   // bytes of the byte order mark matched so far at the start
    bool started_ = false;       // past the place where a byte order mark may be
    bool after_cr_ = false;      // the last byte read was a CR that ended a line
    std::string line_;           // the line being read
    std::string data_;           // the event's data so far, each line followed by LF
    std::string type_;           // the event's type field, empty when none
    std::optional<std::string> id_;  // the id field of the current block, when one was read
    std::string oversized_;
};

}  // namespace neomcp::http
