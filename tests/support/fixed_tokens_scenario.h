// What the socket scenarios of the endpoint protected by fixed tokens share: an HttpServer with fixed_tokens over the
// items of resource_scenario.h, and the tables the tests give it. Header only; every name lives in neomcp::scenario.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "neomcp/http/fixed_tokens.h"
#include "support/resource_scenario.h"

namespace neomcp::scenario {

using http::FixedToken;
using http::FixedTokenTable;

// A table of `tokens`; throws when it is refused.
inline FixedTokenTable fixed_table(std::vector<FixedToken> tokens) {
    auto made = FixedTokenTable::create(std::move(tokens));
    if (const auto* error = std::get_if<http::FixedTokenError>(&made)) {
        throw std::runtime_error("the table is refused: " + std::string(http::to_string(error->problem)));
    }
    return std::get<FixedTokenTable>(std::move(made));
}

inline FixedToken fixed(std::string token, std::string subject, std::vector<std::string> scopes,
                        std::optional<SystemClock::time_point> expires = std::nullopt) {
    return FixedToken{std::move(token), std::move(subject), std::move(scopes), expires};
}

// An HttpServer on a loopback port with fixed tokens (required_scopes mcp:use), over the items of resource_scenario.h
// and those `more` registers.
struct FixedHarness {
    explicit FixedHarness(std::vector<FixedToken> tokens, std::function<void(HttpServerOptions&)> tweak = {},
                          std::function<void(Server&)> more = {})
        : server(server_options()) {
        register_items(server, items);
        if (more) more(server);
        HttpServerOptions options = quick_options();
        http::FixedTokenOptions fixed_options;
        fixed_options.tokens = fixed_table(std::move(tokens));
        fixed_options.required_scopes = {"mcp:use"};
        fixed_options.clock = clock.function();
        options.fixed_tokens = std::move(fixed_options);
        if (tweak) tweak(options);
        http = std::make_unique<HttpServer>(server, std::move(options));
        auto started = http->start();
        if (!started) throw std::runtime_error("start failed: " + started.error);
        port = *started.value;
    }
    ~FixedHarness() {
        if (http) http->stop();
    }
    FixedHarness(const FixedHarness&) = delete;
    FixedHarness& operator=(const FixedHarness&) = delete;

    // Replaces the table; throws when it is refused.
    void replace(std::vector<FixedToken> tokens) {
        auto replaced = http->replace_fixed_tokens(fixed_table(std::move(tokens)));
        if (!replaced) throw std::runtime_error("replace failed: " + replaced.error);
    }

    Items items;
    FakeClock clock;
    Server server;
    std::unique_ptr<HttpServer> http;
    std::uint16_t port = 0;
};

// What whoami saw, from its answer.
inline json who_of(const Reply& reply) { return json::parse(text_of(reply)); }

}  // namespace neomcp::scenario
