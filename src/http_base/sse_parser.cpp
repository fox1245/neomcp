// Reading server-sent events (neomcp/http/sse_parser.h).
#include "neomcp/http/sse_parser.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace neomcp::http {

namespace {

constexpr char cr = '\r';
constexpr char lf = '\n';
constexpr char colon = ':';
constexpr char space = ' ';
constexpr char nul = '\0';
constexpr std::array<char, 3> byte_order_mark{'\xEF', '\xBB', '\xBF'};

constexpr std::string_view field_data = "data";
constexpr std::string_view field_event = "event";
constexpr std::string_view field_id = "id";
constexpr std::string_view field_retry = "retry";

bool ascii_digits(std::string_view text) noexcept {
    if (text.empty()) return false;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

}  // namespace

SseParser::SseParser(std::size_t max_event_bytes) : max_event_bytes_(max_event_bytes) {
    if (max_event_bytes == 0) throw std::invalid_argument("an event stream parser needs a bound above zero");
}

std::size_t SseParser::line_room() const noexcept {
    const std::size_t limit = max_event_bytes_ + line_overhead;
    const std::size_t held = data_.size() + line_.size();
    return held >= limit ? 0 : limit - held;
}

SseParser::Status SseParser::feed(std::string_view bytes, const std::function<void(SseItem)>& sink) {
    if (status_ != Status::reading) return status_;
    std::size_t at = 0;
    // A byte order mark is dropped only at the very start of the stream; its bytes may come in several pieces.
    while (!started_ && at < bytes.size()) {
        if (bytes[at] == byte_order_mark[bom_seen_]) {
            ++bom_seen_;
            ++at;
            if (bom_seen_ == byte_order_mark.size()) started_ = true;
            continue;
        }
        started_ = true;
        if (bom_seen_ > 0) {
            // What looked like the start of a mark is content.
            const std::string held(byte_order_mark.data(), bom_seen_);
            bom_seen_ = 0;
            feed(held, sink);
            if (status_ != Status::reading) return status_;
        }
    }
    while (at < bytes.size()) {
        const char c = bytes[at];
        if (after_cr_) {
            after_cr_ = false;
            if (c == lf) {
                ++at;
                continue;
            }
        }
        if (c == cr || c == lf) {
            after_cr_ = c == cr;
            ++at;
            std::string text = std::move(line_);
            line_.clear();
            line(text, sink);
            if (status_ != Status::reading) return status_;
            continue;
        }
        // The run of bytes up to the next line end joins the pending line, within the bound.
        std::size_t end = at;
        while (end < bytes.size() && bytes[end] != cr && bytes[end] != lf) ++end;
        const std::string_view run = bytes.substr(at, end - at);
        if (const std::size_t room = line_room(); run.size() > room) {
            line_.append(run.substr(0, room + 1));
            oversized_ = std::move(data_);
            oversized_ += line_;
            data_.clear();
            line_.clear();
            status_ = Status::too_large;
            return status_;
        }
        line_.append(run);
        at = end;
    }
    return status_;
}

void SseParser::line(std::string_view text, const std::function<void(SseItem)>& sink) {
    if (text.empty()) {
        dispatch(sink);
        return;
    }
    if (text.front() == colon) return;  // a comment
    std::string_view name = text;
    std::string_view value;
    if (const auto split = text.find(colon); split != std::string_view::npos) {
        name = text.substr(0, split);
        value = text.substr(split + 1);
        if (!value.empty() && value.front() == space) value.remove_prefix(1);
    }
    if (name == field_data) {
        // The data, without the line feed that the dispatch drops, stays within the bound.
        if (value.size() > max_event_bytes_ || data_.size() > max_event_bytes_ - value.size()) {
            oversized_ = std::move(data_);
            oversized_.append(value);
            data_.clear();
            status_ = Status::too_large;
            return;
        }
        data_.append(value);
        data_.push_back(lf);
    } else if (name == field_event) {
        type_.assign(value);
    } else if (name == field_id) {
        if (value.find(nul) == std::string_view::npos) id_ = std::string(value);
    } else if (name == field_retry) {
        if (!ascii_digits(value)) return;
        std::uint64_t milliseconds = 0;
        const auto [stop, error] = std::from_chars(value.data(), value.data() + value.size(), milliseconds);
        using Rep = std::chrono::milliseconds::rep;
        if (error != std::errc() || stop != value.data() + value.size() ||
            milliseconds > static_cast<std::uint64_t>(std::numeric_limits<Rep>::max())) {
            return;
        }
        sink(SseRetry{std::chrono::milliseconds(static_cast<Rep>(milliseconds))});
    }
}

void SseParser::dispatch(const std::function<void(SseItem)>& sink) {
    if (id_) {
        sink(SseLastEventId{std::move(*id_)});
        id_.reset();
    }
    if (data_.empty()) {
        type_.clear();
        return;
    }
    data_.pop_back();  // the LF after the last data line
    SseEvent event;
    event.type = type_.empty() ? std::string(sse_default_event_type) : std::move(type_);
    event.data = std::move(data_);
    data_.clear();
    type_.clear();
    sink(std::move(event));
}

SseParser::Status SseParser::finish() {
    if (status_ == Status::reading) {
        status_ = Status::ended;
        line_.clear();
        data_.clear();
        type_.clear();
        id_.reset();
    }
    return status_;
}

std::string SseParser::take_oversized() { return std::exchange(oversized_, std::string()); }

}  // namespace neomcp::http
