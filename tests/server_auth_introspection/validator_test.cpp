// The validator against a scripted introspection endpoint on a loopback port: the bytes of its requests, the
// authentication, the verdicts, the bounds on the answer and the time, an endpoint that cannot be reached, and the
// secrets that stay out of every text.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/http/form.h"
#include "support.h"

namespace {

using namespace introspection_test;
using http_test::Request;
using http_test::ScriptedHttpServer;
using neomcp::http::IntrospectionBasicAuth;
using neomcp::http::IntrospectionBearerAuth;
using neomcp::http::IntrospectionPostAuth;
using SystemClock = std::chrono::system_clock;

// The bytes the first test pins down. The expected values are written out, not computed with the code under test.
constexpr const char* token_with_marks = "abc.DEF-1_2~3+/=";
constexpr const char* token_form = "token=abc.DEF-1_2~3%2B%2F%3D&token_type_hint=access_token";
constexpr const char* basic_line = "Authorization: Basic YXBwJTNBb25lOnMlMkJjcmV0JTI1";
constexpr const char* post_fields = "&client_id=app%3Aone&client_secret=s%2Bcret%25";

ScriptedHttpServer::Script always(json answer, int status = 200) {
    return [answer = std::move(answer), status](const Request&) { return http_test::json_reply(status, answer); };
}

// A reply with the given head and body bytes, closing the connection after.
http_test::Reply raw(std::string bytes) {
    http_test::Reply reply;
    reply.send(std::move(bytes));
    return reply;
}

TEST(IntrospectionValidator, AnActiveAnswerGivesTheAuthorization) {
    ScriptedHttpServer server(always(active_answer()));
    auto log = std::make_shared<EventLog>();
    auto validator = make(options_for(server.url(introspection_path), log));
    const TokenVerdict verdict = ask(*validator, "tok-1");
    const neomcp::Authorization* granted = authorization_of(verdict);
    ASSERT_NE(granted, nullptr);
    EXPECT_EQ(granted->issuer, issuer);
    EXPECT_EQ(granted->subject, std::optional<std::string>("alice"));
    EXPECT_EQ(granted->scopes, (std::vector<std::string>{"mcp:use", "files:read"}));
    EXPECT_EQ(granted->audiences, (std::vector<std::string>{resource}));
    EXPECT_EQ(granted->expires, std::optional<SystemClock::time_point>(SystemClock::time_point(std::chrono::seconds(4102444800LL))));
    EXPECT_EQ(log->all(), (std::vector<IntrospectionEvent>{IntrospectionEvent::active}));
    EXPECT_EQ(server.count(), 1U);
}

TEST(IntrospectionValidator, AnInactiveAnswerIsInvalid) {
    ScriptedHttpServer server(always(json{{"active", false}}));
    auto log = std::make_shared<EventLog>();
    auto validator = make(options_for(server.url(introspection_path), log));
    EXPECT_TRUE(is_refusal(ask(*validator, "tok-1"), TokenRefusal::invalid));
    EXPECT_EQ(log->all(), (std::vector<IntrospectionEvent>{IntrospectionEvent::inactive}));
}

TEST(IntrospectionValidator, AnAnswerThatIsNotActiveTrueIsNeverAccepted) {
    for (const char* body : {R"({"active":"true"})", R"({"active":1})", R"({})", R"({"scope":"a","aud":["x"]})"}) {
        ScriptedHttpServer server([body](const Request&) {
            return http_test::raw_reply(200, {{"Content-Type", "application/json"}}, body);
        });
        auto log = std::make_shared<EventLog>();
        auto validator = make(options_for(server.url(introspection_path), log));
        EXPECT_TRUE(is_refusal(ask(*validator, "tok-1"), TokenRefusal::invalid)) << body;
    }
}

TEST(IntrospectionValidator, TheBasicRequestIsExactlyThis) {
    ScriptedHttpServer server(always(active_answer()));
    auto log = std::make_shared<EventLog>();
    auto validator = make(options_for(server.url(introspection_path), log));
    ASSERT_NE(authorization_of(ask(*validator, token_with_marks)), nullptr);
    const std::vector<Request> seen = server.requests();
    ASSERT_EQ(seen.size(), 1U);
    const Request& request = seen.front();
    EXPECT_EQ(request.method, "POST");
    EXPECT_EQ(request.target, introspection_path);
    EXPECT_EQ(request.body, token_form);
    EXPECT_TRUE(request.has_line(basic_line)) << request.head;
    EXPECT_EQ(request.fields_named("Authorization").size(), 1U);
    EXPECT_TRUE(request.has_line("Content-Type: application/x-www-form-urlencoded")) << request.head;
    EXPECT_TRUE(request.has_line("Accept: application/json")) << request.head;
    EXPECT_EQ(request.field("Content-Length"), std::optional<std::string>(std::to_string(std::string(token_form).size())));
    // The token is in the body alone: not in the line, not in a field.
    EXPECT_EQ(request.head.find("abc.DEF"), std::string::npos) << request.head;
}

TEST(IntrospectionValidator, ThePostRequestCarriesTheCredentialsInTheForm) {
    ScriptedHttpServer server(always(active_answer()));
    auto log = std::make_shared<EventLog>();
    auto options = options_for(server.url(introspection_path), log);
    options.authentication = IntrospectionPostAuth{client_id, client_secret};
    auto validator = make(std::move(options));
    ASSERT_NE(authorization_of(ask(*validator, token_with_marks)), nullptr);
    const Request request = server.requests().front();
    EXPECT_EQ(request.body, std::string(token_form) + post_fields);
    EXPECT_FALSE(request.field("Authorization").has_value()) << request.head;
}

TEST(IntrospectionValidator, TheBearerRequestCarriesTheResourcesOwnToken) {
    ScriptedHttpServer server(always(active_answer()));
    auto log = std::make_shared<EventLog>();
    auto options = options_for(server.url(introspection_path), log);
    options.authentication = IntrospectionBearerAuth{"svc-token.123"};
    auto validator = make(std::move(options));
    ASSERT_NE(authorization_of(ask(*validator, token_with_marks)), nullptr);
    const Request request = server.requests().front();
    EXPECT_EQ(request.body, token_form);
    EXPECT_TRUE(request.has_line("Authorization: Bearer svc-token.123")) << request.head;
    EXPECT_EQ(request.fields_named("Authorization").size(), 1U);
}

TEST(IntrospectionValidator, ManyTokensAtOnceEachGetTheirOwnVerdict) {
    ScriptedHttpServer server([](const Request& request) {
        auto fields = neomcp::http::form_decode(request.body);
        std::string token;
        if (const auto* list = std::get_if<std::vector<neomcp::http::FormField>>(&fields)) {
            token = std::string(neomcp::http::find_unique_field(*list, "token").value);
        }
        json answer = active_answer();
        answer["sub"] = token;
        return http_test::json_reply(200, answer);
    });
    auto log = std::make_shared<EventLog>();
    auto validator = make(options_for(server.url(introspection_path), log));
    std::vector<std::future<TokenVerdict>> pending;
    for (int i = 0; i < 12; ++i) {
        pending.push_back(std::async(std::launch::async, [&validator, i] { return ask(*validator, "token-" + std::to_string(i)); }));
    }
    for (int i = 0; i < 12; ++i) {
        const TokenVerdict verdict = pending[static_cast<std::size_t>(i)].get();
        const neomcp::Authorization* granted = authorization_of(verdict);
        ASSERT_NE(granted, nullptr) << i;
        EXPECT_EQ(granted->subject, std::optional<std::string>("token-" + std::to_string(i)));
    }
    EXPECT_EQ(server.count(), 12U);
}

TEST(IntrospectionValidator, AnythingButStatusOkIsUnavailable) {
    for (const int status : {400, 401, 403, 404, 500, 503}) {
        ScriptedHttpServer server(always(active_answer(), status));
        auto log = std::make_shared<EventLog>();
        auto validator = make(options_for(server.url(introspection_path), log));
        EXPECT_TRUE(is_refusal(ask(*validator, "tok-1"), TokenRefusal::unavailable)) << status;
        EXPECT_EQ(log->all(), (std::vector<IntrospectionEvent>{IntrospectionEvent::unexpected_status})) << status;
    }
}

TEST(IntrospectionValidator, ARedirectIsAnAnswerNotAWayToGo) {
    ScriptedHttpServer elsewhere(always(active_answer()));
    for (const int status : {301, 302, 307, 308}) {
        ScriptedHttpServer server([&elsewhere, status](const Request&) {
            return http_test::empty_reply(status, {{"Location", elsewhere.url("/other")}});
        });
        auto log = std::make_shared<EventLog>();
        auto validator = make(options_for(server.url(introspection_path), log));
        EXPECT_TRUE(is_refusal(ask(*validator, "tok-1"), TokenRefusal::unavailable)) << status;
        EXPECT_EQ(log->all(), (std::vector<IntrospectionEvent>{IntrospectionEvent::unexpected_status})) << status;
    }
    EXPECT_EQ(elsewhere.count(), 0U);
}

TEST(IntrospectionValidator, ABodyThatIsNotAJsonObjectIsUnavailable) {
    for (const char* body : {"<html>login</html>", "[]", "true", ""}) {
        ScriptedHttpServer server([body](const Request&) {
            return http_test::raw_reply(200, {{"Content-Type", "application/json"}}, body);
        });
        auto log = std::make_shared<EventLog>();
        auto validator = make(options_for(server.url(introspection_path), log));
        EXPECT_TRUE(is_refusal(ask(*validator, "tok-1"), TokenRefusal::unavailable)) << body;
        EXPECT_EQ(log->all(), (std::vector<IntrospectionEvent>{IntrospectionEvent::not_an_object})) << body;
    }
}

// An answer of exactly `size` bytes: an active token with a padding member.
std::string answer_of_size(std::size_t size) {
    std::string body = R"({"active":true,"pad":")";
    body.append(size - body.size() - 2, 'p');
    body += "\"}";
    return body;
}

TEST(IntrospectionValidator, TheBodyBoundIsExactlyTheBound) {
    constexpr std::size_t limit = 300;
    for (const bool announced : {true, false}) {
        std::atomic<std::size_t> size{limit};
        ScriptedHttpServer server(nullptr);
        server.set_script([&size, announced](const Request&) {
            const std::string body = answer_of_size(size.load());
            if (announced) return http_test::raw_reply(200, {{"Content-Type", "application/json"}}, body);
            return raw(http_test::head(200, {{"Content-Type", "application/json"}}, std::nullopt) + body);
        });
        auto log = std::make_shared<EventLog>();
        auto options = options_for(server.url(introspection_path), log);
        options.bounds.max_response_bytes = limit;
        auto validator = make(std::move(options));
        EXPECT_NE(authorization_of(ask(*validator, "tok-1")), nullptr) << "at the bound, announced " << announced;
        size = limit + 1;
        EXPECT_TRUE(is_refusal(ask(*validator, "tok-2"), TokenRefusal::unavailable)) << "past the bound, announced " << announced;
        EXPECT_EQ(log->all(), (std::vector<IntrospectionEvent>{IntrospectionEvent::active, IntrospectionEvent::response_too_large}))
            << announced;
    }
}

TEST(IntrospectionValidator, AHugeBodyIsCutOffNotRead) {
    ScriptedHttpServer server([](const Request&) {
        http_test::Reply reply;
        reply.send(http_test::head(200, {{"Content-Type", "application/json"}}, std::nullopt));
        const std::string chunk(64 * 1024, 'x');
        for (int i = 0; i < 64; ++i) reply.send(chunk);
        return reply;
    });
    auto log = std::make_shared<EventLog>();
    auto validator = make(options_for(server.url(introspection_path), log));
    EXPECT_TRUE(is_refusal(ask(*validator, "tok-1"), TokenRefusal::unavailable));
    EXPECT_EQ(log->all(), (std::vector<IntrospectionEvent>{IntrospectionEvent::response_too_large}));
}

TEST(IntrospectionValidator, HeaderFieldsBeyondTheirBoundAreUnavailable) {
    ScriptedHttpServer server([](const Request&) {
        return http_test::raw_reply(200, {{"Content-Type", "application/json"}, {"X-Padding", std::string(4096, 'h')}},
                                    active_answer().dump());
    });
    auto log = std::make_shared<EventLog>();
    auto options = options_for(server.url(introspection_path), log);
    options.bounds.max_response_header_bytes = 1024;
    auto validator = make(std::move(options));
    EXPECT_TRUE(is_refusal(ask(*validator, "tok-1"), TokenRefusal::unavailable));
    EXPECT_EQ(log->all(), (std::vector<IntrospectionEvent>{IntrospectionEvent::response_too_large}));
}

TEST(IntrospectionValidator, ASlowAnswerIsUnavailableAfterTheTimeout) {
    ScriptedHttpServer server(nullptr);
    auto never = std::make_shared<http_test::Gate>();
    server.set_script([never](const Request&) {
        http_test::Reply reply;
        reply.wait(never);
        reply.send(http_test::head(200, {}, 2) + "{}");
        return reply;
    });
    auto log = std::make_shared<EventLog>();
    auto options = options_for(server.url(introspection_path), log);
    options.bounds.request_timeout = 300ms;
    options.bounds.connect_timeout = 200ms;
    auto validator = make(std::move(options));
    const auto started = std::chrono::steady_clock::now();
    EXPECT_TRUE(is_refusal(ask(*validator, "tok-1"), TokenRefusal::unavailable));
    EXPECT_GE(std::chrono::steady_clock::now() - started, 250ms);
    EXPECT_EQ(log->all(), (std::vector<IntrospectionEvent>{IntrospectionEvent::transport_failed}));
}

TEST(IntrospectionValidator, ABodyThatStopsHalfWayIsUnavailableAfterTheTimeout) {
    ScriptedHttpServer server(nullptr);
    auto never = std::make_shared<http_test::Gate>();
    server.set_script([never](const Request&) {
        http_test::Reply reply;
        reply.send(http_test::head(200, {{"Content-Type", "application/json"}}, 100) + R"({"active":true,)");
        reply.wait(never);
        return reply;
    });
    auto log = std::make_shared<EventLog>();
    auto options = options_for(server.url(introspection_path), log);
    options.bounds.request_timeout = 300ms;
    options.bounds.connect_timeout = 200ms;
    auto validator = make(std::move(options));
    EXPECT_TRUE(is_refusal(ask(*validator, "tok-1"), TokenRefusal::unavailable));
    EXPECT_EQ(log->all(), (std::vector<IntrospectionEvent>{IntrospectionEvent::transport_failed}));
}

TEST(IntrospectionValidator, AnEndpointThatCannotBeReachedIsUnavailableNotInvalid) {
    std::string url;
    {
        ScriptedHttpServer gone(always(active_answer()));
        url = gone.url(introspection_path);
        gone.stop();
    }
    auto log = std::make_shared<EventLog>();
    auto validator = make(options_for(url, log));
    EXPECT_TRUE(is_refusal(ask(*validator, "tok-1"), TokenRefusal::unavailable));
    EXPECT_EQ(log->all(), (std::vector<IntrospectionEvent>{IntrospectionEvent::transport_failed}));
}

TEST(IntrospectionValidator, RequestsInProgressAreBounded) {
    ScriptedHttpServer server(nullptr);
    auto open = std::make_shared<http_test::Gate>();
    server.set_script([open](const Request&) {
        http_test::Reply reply;
        reply.wait(open);
        reply.send(http_test::head(200, {{"Content-Type", "application/json"}}, active_answer().dump().size()) +
                   active_answer().dump());
        return reply;
    });
    auto log = std::make_shared<EventLog>();
    auto options = options_for(server.url(introspection_path), log);
    options.bounds.max_pending = 1;
    auto validator = make(std::move(options));
    auto first = std::async(std::launch::async, [&] { return ask(*validator, "tok-1"); });
    ASSERT_TRUE(server.wait_for(1));
    EXPECT_TRUE(is_refusal(ask(*validator, "tok-2"), TokenRefusal::unavailable));
    EXPECT_EQ(server.count(), 1U);
    open->open();
    EXPECT_NE(authorization_of(first.get()), nullptr);
    EXPECT_EQ(log->all(), (std::vector<IntrospectionEvent>{IntrospectionEvent::overloaded, IntrospectionEvent::active}));
    // The place is free again.
    EXPECT_NE(authorization_of(ask(*validator, "tok-3")), nullptr);
}

TEST(IntrospectionValidator, DroppingTheValidatorAnswersTheRequestsThatWait) {
    ScriptedHttpServer server(nullptr);
    auto never = std::make_shared<http_test::Gate>();
    server.set_script([never](const Request&) {
        http_test::Reply reply;
        reply.wait(never);
        return reply;
    });
    auto log = std::make_shared<EventLog>();
    auto validator = make(options_for(server.url(introspection_path), log));
    auto verdict = std::make_shared<std::promise<TokenVerdict>>();
    auto future = verdict->get_future();
    validator->validate(neomcp::http::TokenCheck{"tok-1", resource}, [verdict](TokenVerdict given) {
        verdict->set_value(std::move(given));
    });
    ASSERT_TRUE(server.wait_for(1));
    validator.reset();
    ASSERT_EQ(future.wait_for(20s), std::future_status::ready);
    EXPECT_TRUE(is_refusal(future.get(), TokenRefusal::unavailable));
    EXPECT_EQ(log->all(), (std::vector<IntrospectionEvent>{IntrospectionEvent::stopped}));
}

TEST(IntrospectionValidator, AReplyThatDropsTheValidatorDoesNotJoinItself) {
    ScriptedHttpServer server(always(active_answer()));
    auto log = std::make_shared<EventLog>();
    auto holder = std::make_shared<std::shared_ptr<neomcp::http::TokenValidator>>(make(options_for(server.url(introspection_path), log)));
    std::promise<void> dropped;
    auto done = dropped.get_future();
    (*holder)->validate(neomcp::http::TokenCheck{"tok-1", resource}, [holder, &dropped](TokenVerdict) {
        holder->reset();
        dropped.set_value();
    });
    holder.reset();
    ASSERT_EQ(done.wait_for(20s), std::future_status::ready);
}

TEST(IntrospectionValidator, WithoutAnExpiryTheValidatorsClockDecides) {
    ScriptedHttpServer server(always(json{{"active", true}, {"aud", resource}}));
    auto log = std::make_shared<EventLog>();
    auto options = options_for(server.url(introspection_path), log);
    const SystemClock::time_point fixed(std::chrono::seconds(1'800'000'000));
    options.clock = [fixed] { return fixed; };
    options.bounds.default_lifetime = 30s;
    auto validator = make(std::move(options));
    const TokenVerdict verdict = ask(*validator, "tok-1");
    const neomcp::Authorization* granted = authorization_of(verdict);
    ASSERT_NE(granted, nullptr);
    EXPECT_EQ(granted->expires, std::optional<SystemClock::time_point>(fixed + 30s));
}

// Sets an environment variable for the length of a test and unsets it after. The validator reads the environment
// only when asked to, so no other test depends on it.
class EnvironmentVariable {
public:
    EnvironmentVariable(std::string name, const std::string& value) : name_(std::move(name)) { set(value); }
    ~EnvironmentVariable() { unset(); }
    EnvironmentVariable(const EnvironmentVariable&) = delete;
    EnvironmentVariable& operator=(const EnvironmentVariable&) = delete;

private:
    void set(const std::string& value) const {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), value.c_str());
#else
        setenv(name_.c_str(), value.c_str(), 1);
#endif
    }
    void unset() const {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), "");
#else
        unsetenv(name_.c_str());
#endif
    }
    std::string name_;
};

TEST(IntrospectionValidator, NoProxyIsUsedUnlessTheEnvironmentIsAskedFor) {
    ScriptedHttpServer server(always(active_answer()));
    // A proxy that nothing listens on: a request that goes through it cannot be answered.
    const EnvironmentVariable proxy("http_proxy", "http://127.0.0.1:1");
    // A list of hosts that bypass the proxy would hide it: make sure there is none.
    const EnvironmentVariable no_proxy_lower("no_proxy", "");
    const EnvironmentVariable no_proxy_upper("NO_PROXY", "");
    for (const bool from_environment : {false, true}) {
        auto log = std::make_shared<EventLog>();
        auto options = options_for(server.url(introspection_path), log);
        options.use_proxy_from_environment = from_environment;
        options.bounds.request_timeout = 3s;
        options.bounds.connect_timeout = 2s;
        auto validator = make(std::move(options));
        const TokenVerdict verdict = ask(*validator, "tok-1");
        if (from_environment) {
            EXPECT_TRUE(is_refusal(verdict, TokenRefusal::unavailable));
        } else {
            EXPECT_NE(authorization_of(verdict), nullptr);
        }
    }
}

TEST(IntrospectionValidator, OptionsThatCannotServeAreRefusedWithoutTheirSecrets) {
    const std::string secret = "very-secret-value-9f3";
    const std::string password = "pw-in-the-url-77";
    struct Case {
        std::string what;
        std::function<void(neomcp::http::IntrospectionOptions&)> tweak;
    };
    const std::vector<Case> cases = {
        {"no endpoint", [](auto& o) { o.endpoint.clear(); }},
        {"not a URL", [](auto& o) { o.endpoint = "introspect"; }},
        {"ftp", [](auto& o) { o.endpoint = "ftp://127.0.0.1/introspect"; }},
        {"plain http to another host", [](auto& o) { o.endpoint = "http://auth.example/introspect"; }},
        {"user information", [&](auto& o) { o.endpoint = "https://user:" + password + "@auth.example/introspect"; }},
        {"user information on loopback", [&](auto& o) { o.endpoint = "http://user:" + password + "@127.0.0.1/i"; }},
        {"a fragment", [](auto& o) { o.endpoint = "https://auth.example/introspect#x"; }},
        {"no host", [](auto& o) { o.endpoint = "https://"; }},
        {"no issuer", [](auto& o) { o.issuer.clear(); }},
        {"basic without id", [&](auto& o) { o.authentication = IntrospectionBasicAuth{"", secret}; }},
        {"basic without secret", [](auto& o) { o.authentication = IntrospectionBasicAuth{"app", ""}; }},
        {"post without secret", [](auto& o) { o.authentication = IntrospectionPostAuth{"app", ""}; }},
        {"post without id", [&](auto& o) { o.authentication = IntrospectionPostAuth{"", secret}; }},
        {"empty bearer", [](auto& o) { o.authentication = IntrospectionBearerAuth{""}; }},
        {"bearer with a line break", [&](auto& o) { o.authentication = IntrospectionBearerAuth{secret + "\r\nX: y"}; }},
        {"bearer with a newline", [&](auto& o) { o.authentication = IntrospectionBearerAuth{secret + "\n"}; }},
        {"no response bytes", [](auto& o) { o.bounds.max_response_bytes = 0; }},
        {"no timeout", [](auto& o) { o.bounds.request_timeout = 0ms; }},
        {"no connect timeout", [](auto& o) { o.bounds.connect_timeout = 0ms; }},
        {"connect over total", [](auto& o) { o.bounds.connect_timeout = o.bounds.request_timeout + 1ms; }},
        {"no header bytes", [](auto& o) { o.bounds.max_response_header_bytes = 0; }},
        {"no pending", [](auto& o) { o.bounds.max_pending = 0; }},
        {"no lifetime", [](auto& o) { o.bounds.default_lifetime = 0s; }},
    };
    for (const Case& refused : cases) {
        neomcp::http::IntrospectionOptions options;
        options.endpoint = "https://auth.example/introspect";
        options.issuer = issuer;
        options.authentication = IntrospectionBasicAuth{"app", secret};
        refused.tweak(options);
        auto made = neomcp::http::make_introspection_validator(std::move(options));
        EXPECT_FALSE(made) << refused.what;
        EXPECT_FALSE(made.error.empty()) << refused.what;
        EXPECT_EQ(made.error.find(secret), std::string::npos) << refused.what << ": " << made.error;
        EXPECT_EQ(made.error.find(password), std::string::npos) << refused.what << ": " << made.error;
        EXPECT_EQ(made.error.find("auth.example"), std::string::npos) << refused.what << ": " << made.error;
    }
}

TEST(IntrospectionValidator, LoopbackAndHttpsEndpointsAreAccepted) {
    for (const char* endpoint : {"https://auth.example/introspect", "https://auth.example:8443/i?x=1",
                                 "http://127.0.0.1:9/i", "http://localhost/i", "http://[::1]:9/i",
                                 "http://127.1.2.3/i"}) {
        neomcp::http::IntrospectionOptions options;
        options.endpoint = endpoint;
        options.issuer = issuer;
        options.authentication = IntrospectionBasicAuth{"app", "secret"};
        auto made = neomcp::http::make_introspection_validator(std::move(options));
        EXPECT_TRUE(made) << endpoint << ": " << made.error;
    }
}

TEST(IntrospectionValidator, NeitherTheTokenNorTheSecretIsInAnyReport) {
    const std::string token = "tok-7c1d-must-not-leak";
    const std::string secret = "client-secret-b83e-must-not-leak";
    std::vector<std::string> texts;
    for (const IntrospectionEvent event :
         {IntrospectionEvent::active, IntrospectionEvent::inactive, IntrospectionEvent::malformed,
          IntrospectionEvent::issuer_mismatch, IntrospectionEvent::transport_failed,
          IntrospectionEvent::unexpected_status, IntrospectionEvent::response_too_large,
          IntrospectionEvent::not_an_object, IntrospectionEvent::overloaded, IntrospectionEvent::stopped}) {
        texts.emplace_back(neomcp::http::to_string(event));
    }
    // Every failure path: a refused status, an unreachable endpoint, an inactive and a malformed answer.
    std::string gone;
    {
        ScriptedHttpServer closed(always(active_answer()));
        gone = closed.url(introspection_path);
        closed.stop();
    }
    ScriptedHttpServer refusing(always(json{{"error", token + secret}}, 401));
    ScriptedHttpServer inactive(always(json{{"active", false}, {"token", token}}));
    for (const std::string& url : {gone, refusing.url(introspection_path), inactive.url(introspection_path)}) {
        for (const bool as_bearer : {false, true}) {
            auto log = std::make_shared<EventLog>();
            auto options = options_for(url, log);
            if (as_bearer) {
                options.authentication = IntrospectionBearerAuth{secret};
            } else {
                options.authentication = IntrospectionBasicAuth{"app", secret};
            }
            auto validator = make(std::move(options));
            const TokenVerdict verdict = ask(*validator, token);
            EXPECT_EQ(authorization_of(verdict), nullptr);
            for (const IntrospectionEvent event : log->all()) texts.emplace_back(neomcp::http::to_string(event));
        }
    }
    // Refusals at creation, with the secret in scope.
    neomcp::http::IntrospectionOptions options;
    options.endpoint = "https://user:" + secret + "@auth.example/i";
    options.issuer = issuer;
    options.authentication = IntrospectionBearerAuth{secret + "\r\n"};
    texts.push_back(neomcp::http::make_introspection_validator(options).error);
    options.endpoint = "https://auth.example/i";
    texts.push_back(neomcp::http::make_introspection_validator(options).error);
    for (const std::string& text : texts) {
        EXPECT_EQ(text.find(token), std::string::npos) << text;
        EXPECT_EQ(text.find(secret), std::string::npos) << text;
        EXPECT_EQ(text.find("must-not-leak"), std::string::npos) << text;
    }
    EXPECT_FALSE(texts.back().empty());
}

}  // namespace
