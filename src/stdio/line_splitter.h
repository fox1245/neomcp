// Cuts a stream of bytes into lines without ever holding more than a bound of one line.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace neomcp::detail {

// A line ends at '\n'; a '\r' before it is dropped; an empty line is skipped. A line longer than `max_line_bytes`
// (measured without the dropped '\r') is handed over truncated to max_line_bytes + 1 bytes, so that the receiver
// sees it is too long, and the rest of it is skipped without being stored.
class LineSplitter {
public:
    explicit LineSplitter(std::size_t max_line_bytes) : keep_(max_line_bytes + 1) {}

    // Calls `on_line(std::string_view)` for each complete line of `chunk`, in order. The view is valid during the call.
    template <class OnLine>
    void feed(std::string_view chunk, OnLine&& on_line) {
        while (!chunk.empty()) {
            const std::size_t end = chunk.find('\n');
            const std::string_view piece = chunk.substr(0, end);
            append(piece);
            if (end == std::string_view::npos) return;
            deliver(on_line);
            chunk.remove_prefix(end + 1);
        }
    }

    // The end of input: a last line without a terminator is a line too.
    template <class OnLine>
    void finish(OnLine&& on_line) {
        if (!line_.empty()) deliver(on_line);
    }

    // The capacity of the buffer, for tests of the bound.
    std::size_t capacity() const noexcept { return line_.capacity(); }

private:
    void append(std::string_view piece) {
        const std::size_t room = keep_ > line_.size() ? keep_ - line_.size() : 0;
        const std::size_t take = piece.size() < room ? piece.size() : room;
        line_.append(piece.data(), take);
        if (take < piece.size()) truncated_ = true;
    }

    template <class OnLine>
    void deliver(OnLine& on_line) {
        // A truncated line is delivered as it is: it is longer than the bound, whatever its kept text ends with.
        if (!truncated_ && !line_.empty() && line_.back() == '\r') line_.pop_back();
        const bool skip = line_.empty();
        if (!skip) on_line(std::string_view(line_));
        line_.clear();
        truncated_ = false;
    }

    std::size_t keep_;
    std::string line_;
    bool truncated_ = false;
};

}  // namespace neomcp::detail
