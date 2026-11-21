// The stream of a session opened by a request that took the table of fixed tokens before a replacement, driven through
// the endpoint's handler: the test replaces the table between the request's lookup and the opening of its stream, and
// waits until the replacement's sweep has run, which finds no stream yet. Only the check made when the stream opens can
// end it, and it does, on every revision that keeps a session.
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "http/endpoint_probe.h"
#include "neomcp/http/endpoint.h"
#include "neomcp/http/fixed_tokens.h"
#include "neomcp/server.h"

namespace {

using namespace std::chrono_literals;
using neomcp::json;
using neomcp::http::FixedToken;
using neomcp::http::FixedTokenTable;

constexpr std::chrono::milliseconds ends_within = 10s;
constexpr const char* first_session_revision = "2025-03-26";

FixedTokenTable table_of(std::vector<FixedToken> tokens) {
    auto made = FixedTokenTable::create(std::move(tokens));
    if (!std::holds_alternative<FixedTokenTable>(made)) throw std::runtime_error("the table is refused");
    return std::get<FixedTokenTable>(std::move(made));
}

struct Rig {
    explicit Rig(std::string revision_served)
        : revision(std::move(revision_served)), server({.name = "probe", .version = "0.0.1"}) {
        neomcp::http::EndpointOptions options;
        options.stream_after = 1h;
        neomcp::http::FixedTokenOptions fixed;
        fixed.tokens = table_of({FixedToken{"gone", "bob", {"mcp:use"}, std::nullopt}});
        fixed.required_scopes = {"mcp:use"};
        options.fixed_tokens = std::move(fixed);
        auto policy = neomcp::http::HostPolicy::create({});
        if (!policy) throw std::runtime_error(policy.error);
        auto made = neomcp::http::detail::EndpointProbe::create(server, std::move(*policy.value), std::move(options), {});
        if (!made) throw std::runtime_error(made.error);
        endpoint = std::move(*made.value);
    }
    ~Rig() { endpoint->stop(); }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    neomcp::http::Request request(const std::string& method, const std::string& accept, std::string body) const {
        neomcp::http::Request made;
        made.method = method;
        made.path = "/mcp";
        made.headers = {{"Host", "localhost"}, {"Accept", accept}, {"Authorization", "Bearer gone"}};
        if (method == "POST") made.headers.add("Content-Type", "application/json");
        if (revision != first_session_revision) made.headers.add("MCP-Protocol-Version", revision);
        if (!session.empty()) made.headers.add("Mcp-Session-Id", session);
        made.body = std::move(body);
        return made;
    }

    // Handles `made` and returns its answer, which every request here gets at once.
    neomcp::http::Response answer(neomcp::http::Request made) {
        std::optional<neomcp::http::Response> answered;
        endpoint->handle(std::move(made), neomcp::http::Responder([&answered](neomcp::http::Response&& response) {
                             answered = std::move(response);
                             return true;
                         }));
        if (!answered) throw std::runtime_error("no answer at once");
        return std::move(*answered);
    }

    void open_session() {
        const json params{{"protocolVersion", revision},
                          {"capabilities", json::object()},
                          {"clientInfo", {{"name", "test-client"}, {"version", "1.0.0"}}}};
        const auto opened = answer(request(
            "POST", "application/json, text/event-stream",
            json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", params}}.dump()));
        const auto id = opened.headers.get("Mcp-Session-Id");
        if (opened.status != 200 || !id) throw std::runtime_error("initialize answered " + std::to_string(opened.status));
        session = std::string(*id);
        const auto ready = answer(request("POST", "application/json, text/event-stream",
                                          json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}.dump()));
        if (ready.status != 202) throw std::runtime_error("initialized answered " + std::to_string(ready.status));
    }

    const std::string revision;
    neomcp::Server server;
    std::unique_ptr<neomcp::http::Endpoint> endpoint;
    std::string session;
};

// Reads `stream` until its end or the time is out; true when it ended.
bool reads_to_its_end(neomcp::http::BodyStream& stream) {
    const auto deadline = std::chrono::steady_clock::now() + ends_within;
    std::string chunk;
    while (std::chrono::steady_clock::now() < deadline) {
        if (stream.read(chunk, 100ms) == neomcp::http::BodyStream::Read::end) return true;
    }
    return false;
}

class FixedTokensProbe : public ::testing::TestWithParam<std::string> {};

TEST_P(FixedTokensProbe, ASessionStreamThatOpensAfterTheSweepForARemovedTokenIsEndedWhenItOpens) {
    Rig rig(GetParam());
    rig.open_session();
    std::atomic<bool> armed{false};
    std::atomic<int> replaced{0};
    neomcp::http::detail::EndpointProbe::observe_fixed_lookups(*rig.endpoint, [&] {
        if (!armed.exchange(false)) return;
        if (rig.endpoint->replace_fixed_tokens(table_of({FixedToken{"other", "carol", {"mcp:use"}, std::nullopt}}))) {
            ++replaced;
        }
        // The sweep has run, and found no stream of the session.
        neomcp::http::detail::EndpointProbe::wait_for_timer(*rig.endpoint);
    });
    armed = true;
    const neomcp::http::Response opened = rig.answer(rig.request("GET", "text/event-stream", {}));
    EXPECT_EQ(replaced.load(), 1);
    ASSERT_EQ(opened.status, 200);
    ASSERT_TRUE(opened.stream);
    EXPECT_TRUE(reads_to_its_end(*opened.stream)) << "the stream opened after the sweep is still open";

    // Opened again, it is refused: the token is gone.
    const neomcp::http::Response again = rig.answer(rig.request("GET", "text/event-stream", {}));
    EXPECT_EQ(again.status, 401);
}

INSTANTIATE_TEST_SUITE_P(FixedTokens, FixedTokensProbe, ::testing::Values("2025-03-26", "2025-06-18", "2025-11-25"),
                         [](const auto& info) {
                             std::string name = info.param;
                             for (char& c : name) {
                                 if (c == '-') c = '_';
                             }
                             return name;
                         });

}  // namespace
