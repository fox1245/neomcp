#include "neomcp/http/sse.h"

#include "neomcp/http/message.h"

namespace neomcp::http {

namespace {

constexpr std::string_view data_field = "data:";
constexpr std::string_view keep_alive_text = "keep-alive";

bool is_blank(char c) noexcept { return c == ' ' || c == '\t'; }

}  // namespace

std::string sse_event(std::string_view payload) {
    std::string frame;
    frame.reserve(payload.size() + data_field.size() + 4);
    std::size_t start = 0;
    while (true) {
        std::size_t end = start;
        while (end < payload.size() && payload[end] != '\n' && payload[end] != '\r') ++end;
        frame += data_field;
        if (end > start) {
            frame += ' ';
            frame.append(payload.substr(start, end - start));
        }
        frame += '\n';
        if (end >= payload.size()) break;
        // Step over one line ending: "\r\n" counts as one.
        start = end + 1;
        if (payload[end] == '\r' && start < payload.size() && payload[start] == '\n') ++start;
    }
    frame += '\n';
    return frame;
}

std::string sse_comment(std::string_view text) {
    std::string frame = ": ";
    frame.reserve(text.size() + 5);
    for (char c : text) frame += (c == '\n' || c == '\r') ? ' ' : c;
    frame += "\n\n";
    return frame;
}

std::string sse_keep_alive() { return sse_comment(keep_alive_text); }

bool is_event_stream_content_type(std::string_view content_type) noexcept {
    const std::size_t semicolon = content_type.find(';');
    std::string_view media = content_type.substr(0, semicolon);
    while (!media.empty() && is_blank(media.front())) media.remove_prefix(1);
    while (!media.empty() && is_blank(media.back())) media.remove_suffix(1);
    return equal_ignoring_case(media, event_stream_media_type);
}

}  // namespace neomcp::http
