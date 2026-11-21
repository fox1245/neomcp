// Helpers of the introspection tests: a validator against a scripted introspection endpoint, the events it reports,
// and a blocking ask.
#pragma once

#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "neomcp/http/introspection.h"
#include "neomcp/http/resource_server.h"
#include "scripted_http_server.h"

namespace introspection_test {

using namespace std::chrono_literals;
using neomcp::json;
using neomcp::http::IntrospectionEvent;
using neomcp::http::TokenRefusal;
using neomcp::http::TokenVerdict;

constexpr const char* issuer = "https://auth.example";
constexpr const char* resource = "http://localhost/mcp";
constexpr const char* introspection_path = "/introspect";
constexpr const char* client_id = "app:one";
constexpr const char* client_secret = "s+cret%";

// The events a validator reported, in order.
class EventLog {
public:
    void add(IntrospectionEvent event) {
        {
            std::lock_guard lock(mutex_);
            events_.push_back(event);
        }
        changed_.notify_all();
    }
    std::vector<IntrospectionEvent> all() {
        std::lock_guard lock(mutex_);
        return events_;
    }
    // Waits until `count` events were reported; false when the bound lapsed first.
    bool wait_for(std::size_t count, std::chrono::milliseconds bound = 20s) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, bound, [&] { return events_.size() >= count; });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<IntrospectionEvent> events_;
};

inline neomcp::http::IntrospectionOptions options_for(const std::string& url, const std::shared_ptr<EventLog>& log) {
    neomcp::http::IntrospectionOptions options;
    options.endpoint = url;
    options.issuer = issuer;
    options.authentication = neomcp::http::IntrospectionBasicAuth{client_id, client_secret};
    options.bounds.request_timeout = 10s;
    options.bounds.connect_timeout = 5s;
    options.observer = [log](IntrospectionEvent event) { log->add(event); };
    return options;
}

inline std::shared_ptr<neomcp::http::TokenValidator> make(neomcp::http::IntrospectionOptions options) {
    auto made = neomcp::http::make_introspection_validator(std::move(options));
    if (!made) throw std::runtime_error("the validator was refused: " + made.error);
    return *made.value;
}

// The verdict of one validate() call; throws when none comes within the bound.
inline TokenVerdict ask(neomcp::http::TokenValidator& validator, const std::string& token,
                        std::chrono::milliseconds bound = 20s) {
    auto verdict = std::make_shared<std::promise<TokenVerdict>>();
    auto future = verdict->get_future();
    auto once = std::make_shared<std::once_flag>();
    validator.validate(neomcp::http::TokenCheck{token, resource}, [verdict, once](TokenVerdict given) {
        std::call_once(*once, [&] { verdict->set_value(std::move(given)); });
    });
    if (future.wait_for(bound) != std::future_status::ready) throw std::runtime_error("no verdict came");
    return future.get();
}

inline bool is_refusal(const TokenVerdict& verdict, TokenRefusal expected) {
    const auto* refusal = std::get_if<TokenRefusal>(&verdict);
    return refusal != nullptr && *refusal == expected;
}

inline const neomcp::Authorization* authorization_of(const TokenVerdict& verdict) {
    return std::get_if<neomcp::Authorization>(&verdict);
}

// The answer of an introspection endpoint that finds the token active.
inline json active_answer() {
    return json{{"active", true}, {"scope", "mcp:use files:read"}, {"client_id", "client-1"}, {"sub", "alice"},
                {"aud", json::array({resource})}, {"iss", issuer}, {"exp", 4102444800LL}};
}

}  // namespace introspection_test
