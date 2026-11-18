#include "neomcp/http/message.h"

#include <algorithm>
#include <atomic>
#include <utility>

namespace neomcp::http {

namespace {

constexpr char fold(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

constexpr int internal_error_status = 500;

}  // namespace

bool equal_ignoring_case(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (fold(left[i]) != fold(right[i])) return false;
    }
    return true;
}

Headers::Headers(std::initializer_list<Field> fields) : fields_(fields) {}

Headers& Headers::add(std::string name, std::string value) {
    fields_.push_back({std::move(name), std::move(value)});
    return *this;
}

Headers& Headers::set(std::string name, std::string value) {
    auto first = std::find_if(fields_.begin(), fields_.end(),
                              [&](const Field& f) { return equal_ignoring_case(f.name, name); });
    if (first == fields_.end()) return add(std::move(name), std::move(value));
    first->name = std::move(name);
    first->value = std::move(value);
    auto rest = std::remove_if(first + 1, fields_.end(),
                               [&](const Field& f) { return equal_ignoring_case(f.name, first->name); });
    fields_.erase(rest, fields_.end());
    return *this;
}

std::size_t Headers::remove(std::string_view name) {
    auto rest = std::remove_if(fields_.begin(), fields_.end(),
                               [&](const Field& f) { return equal_ignoring_case(f.name, name); });
    const auto removed = static_cast<std::size_t>(fields_.end() - rest);
    fields_.erase(rest, fields_.end());
    return removed;
}

std::optional<std::string_view> Headers::get(std::string_view name) const noexcept {
    for (const Field& f : fields_) {
        if (equal_ignoring_case(f.name, name)) return std::string_view(f.value);
    }
    return std::nullopt;
}

std::vector<std::string_view> Headers::get_all(std::string_view name) const {
    std::vector<std::string_view> values;
    for (const Field& f : fields_) {
        if (equal_ignoring_case(f.name, name)) values.emplace_back(f.value);
    }
    return values;
}

std::size_t Headers::count(std::string_view name) const noexcept {
    std::size_t n = 0;
    for (const Field& f : fields_) {
        if (equal_ignoring_case(f.name, name)) ++n;
    }
    return n;
}

Response make_response(int status, std::string_view content_type, std::string body) {
    Response response;
    response.status = status;
    if (!content_type.empty()) response.headers.add("Content-Type", std::string(content_type));
    response.body = std::move(body);
    return response;
}

struct Responder::State {
    explicit State(Sink s) : sink(std::move(s)) {}

    // The last copy of the responder is gone. A request nobody completed is answered 500.
    ~State() {
        if (!done.exchange(true)) {
            try {
                if (sink) sink(make_response(internal_error_status));
            } catch (...) {
            }
        }
    }

    Sink sink;
    std::atomic<bool> done{false};
};

Responder::Responder(Sink sink) : state_(std::make_shared<State>(std::move(sink))) {}

bool Responder::complete(Response response) const {
    if (!state_ || state_->done.exchange(true)) return false;  // a moved-from responder has no state
    try {
        return state_->sink && state_->sink(std::move(response));
    } catch (...) {
        return false;
    }
}

bool Responder::completed() const noexcept { return state_ && state_->done.load(); }

}  // namespace neomcp::http
