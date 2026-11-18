// Server-sent events framing (the text/event-stream format).
//
// Only `data` events and comments are produced: no event names, no ids and no retry hints. Each function returns
// the bytes to append to a response body; an event or a comment is complete when it ends with a blank line, so a
// frame can be written as it is.
#pragma once

#include <string>
#include <string_view>

namespace neomcp::http {

// The media type of an event stream response.
inline constexpr std::string_view event_stream_media_type = "text/event-stream";

// Frames `payload` as one event: one `data:` line per line of the payload, then a blank line. A payload line ends
// at "\n", "\r\n" or "\r" (a receiver splits at all three, so each starts a new `data:` line here). A single-line
// payload gives "data: <payload>\n\n". The receiver joins the lines with "\n", so a payload without a line ending
// round-trips unchanged.
std::string sse_event(std::string_view payload);

// Frames `text` as a comment: ": <text>\n\n". Line endings inside `text` are replaced by spaces, so the result is
// always one comment line followed by a blank line. A receiver ignores comments; they keep an idle connection alive.
std::string sse_comment(std::string_view text);

// The comment the listener writes on an idle event stream.
std::string sse_keep_alive();

// True when a Content-Type field value names the event stream media type: the part before the first ';', trimmed of
// spaces and tabs, equals "text/event-stream" without regard to case. Parameters (a charset) do not matter.
bool is_event_stream_content_type(std::string_view content_type) noexcept;

}  // namespace neomcp::http
