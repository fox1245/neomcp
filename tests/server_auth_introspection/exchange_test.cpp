// The transfer loop on its own, against a scripted peer: the bound on a body that was not announced, which only the
// loop's own check enforces (a body announced as too long is refused before it is read).
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "server_auth_introspection/exchange.h"
#include "support.h"

namespace {

using namespace introspection_test;
using neomcp::http::detail::ExchangeEnd;
using neomcp::http::detail::ExchangeLoop;
using neomcp::http::detail::ExchangeRequest;
using neomcp::http::detail::ExchangeResult;

// What the peer answers: a head without a length, then the body, then the end of the connection.
http_test::Reply unannounced(const std::string& body) {
    http_test::Reply reply;
    reply.send(http_test::head(200, {{"Content-Type", "application/json"}}, std::nullopt) + body);
    return reply;
}

ExchangeResult run(ExchangeLoop& loop, const std::string& url, std::size_t max_body_bytes) {
    ExchangeRequest request;
    request.url = url;
    request.body = "token=x";
    request.total_timeout = 10s;
    request.connect_timeout = 5s;
    request.max_body_bytes = max_body_bytes;
    request.max_header_bytes = 16 * 1024;
    auto result = std::make_shared<std::promise<ExchangeResult>>();
    auto future = result->get_future();
    loop.submit(std::move(request), [result](ExchangeResult ended) { result->set_value(std::move(ended)); });
    if (future.wait_for(20s) != std::future_status::ready) throw std::runtime_error("the exchange did not end");
    return future.get();
}

TEST(IntrospectionExchange, ABodyOfExactlyTheBoundIsKeptWholeAndOneMoreIsNot) {
    constexpr std::size_t bound = 64;
    std::atomic<std::size_t> size{bound};
    http_test::ScriptedHttpServer server(
        [&size](const http_test::Request&) { return unannounced(std::string(size.load(), 'b')); });
    ExchangeLoop loop;
    ASSERT_FALSE(loop.start().has_value());
    const ExchangeResult at_bound = run(loop, server.url(introspection_path), bound);
    EXPECT_EQ(at_bound.end, ExchangeEnd::response);
    EXPECT_EQ(at_bound.status, 200);
    EXPECT_EQ(at_bound.body, std::string(bound, 'b'));
    size = bound + 1;
    const ExchangeResult past = run(loop, server.url(introspection_path), bound);
    EXPECT_EQ(past.end, ExchangeEnd::body_too_large);
    EXPECT_TRUE(past.body.empty());
    // The loop goes on after a transfer it cut off.
    size = bound;
    EXPECT_EQ(run(loop, server.url(introspection_path), bound).end, ExchangeEnd::response);
}

}  // namespace
