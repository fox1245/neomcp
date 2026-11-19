#include "neomcp/http/message.h"

#include <atomic>
#include <mutex>
#include <utility>

namespace neomcp::http {

namespace {

constexpr int internal_error_status = 500;

void call_quietly(const std::function<void()>& callback) noexcept {
    if (!callback) return;
    try {
        callback();
    } catch (...) {
        // A callback that throws must not take down the listener's thread.
    }
}

}  // namespace

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
    // Set once, by the first completion, the destruction of the last copy, or a reported disconnect.
    std::atomic<bool> done{false};

    std::mutex mutex;  // guards the two below
    bool disconnected = false;
    std::function<void()> on_disconnect;
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

void Responder::on_disconnect(std::function<void()> callback) const {
    if (!state_) return;
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->disconnected) {
            state_->on_disconnect = std::move(callback);
            return;
        }
    }
    call_quietly(callback);
}

Responder::Watch Responder::watch() const { return Watch(state_); }

bool Responder::Watch::report_disconnect() const {
    const std::shared_ptr<State> state = state_.lock();
    if (!state || state->done.exchange(true)) return false;
    std::function<void()> callback;
    {
        std::lock_guard lock(state->mutex);
        state->disconnected = true;
        callback = std::move(state->on_disconnect);
        state->on_disconnect = nullptr;
    }
    call_quietly(callback);
    return true;
}

}  // namespace neomcp::http
