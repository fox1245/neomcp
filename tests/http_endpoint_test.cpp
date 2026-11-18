// Tests of the Streamable HTTP endpoint and HttpServer, over real sockets on the loopback interface.
//
// The requests are the ones the official conformance runner sends (both eras on one endpoint), written out with the
// HTTP library's client, which is used here, in the tests only. Every wait is on an event a test controls (a gate a
// tool waits on, a mailbox, the end of a response); the bound on a wait only limits how long a broken build takes to
// fail. The few checks that something does NOT happen wait a short window and say so.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <httplib.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "http/timer.h"
#include "neomcp/http/endpoint.h"
#include "neomcp/http_server.h"
#include "neomcp/server.h"

struct EchoArgs {
    std::string text;
};
NEOMCP_DESCRIBE(EchoArgs, text, "the text to return")

namespace {

using namespace std::chrono_literals;
using neomcp::json;

// Long enough for a loaded machine; reached only when something is broken.
constexpr std::chrono::milliseconds patience = 30s;
// How long something that must happen soon may take in a healthy run.
constexpr std::chrono::milliseconds promptly = 5s;

constexpr const char* modern = "2026-07-28";
constexpr const char* legacy = "2025-11-25";
constexpr const char* path = "/mcp";
constexpr const char* json_type = "application/json";
constexpr const char* both_types = "application/json, text/event-stream";

std::chrono::milliseconds since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
}

template <class T>
class Mailbox {
public:
    void push(T value) {
        {
            std::lock_guard lock(mutex_);
            items_.push_back(std::move(value));
        }
        arrived_.notify_all();
    }
    std::optional<T> pop(std::chrono::milliseconds timeout = patience) {
        std::unique_lock lock(mutex_);
        if (!arrived_.wait_for(lock, timeout, [this] { return !items_.empty(); })) return std::nullopt;
        T value = std::move(items_.front());
        items_.pop_front();
        return value;
    }

private:
    std::mutex mutex_;
    std::condition_variable arrived_;
    std::deque<T> items_;
};

class Counter {
public:
    void add() {
        {
            std::lock_guard lock(mutex_);
            ++value_;
        }
        changed_.notify_all();
    }
    int value() {
        std::lock_guard lock(mutex_);
        return value_;
    }
    bool wait_for_at_least(int n, std::chrono::milliseconds timeout = patience) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return value_ >= n; });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    int value_ = 0;
};

// What a tool waits on: the test opens it, or the call's stop is requested.
class Gate {
public:
    void open() {
        {
            std::lock_guard lock(mutex_);
            opened_ = true;
        }
        changed_.notify_all();
    }
    // True when opened, false when the stop came first.
    bool wait(const std::stop_token& stop) {
        std::stop_callback wake(stop, [this] {
            std::lock_guard lock(mutex_);
            changed_.notify_all();
        });
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return opened_ || stop.stop_requested(); });
        return opened_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool opened_ = false;
};

// ---- The server under test -------------------------------------------------------------------------------------------

neomcp::HttpServerOptions quick_options() {
    neomcp::HttpServerOptions options;
    options.disconnect_poll_interval = 20ms;
    options.endpoint.stream_after = 1h;  // a silent request is answered in JSON unless a test says otherwise
    options.keep_alive_interval = 1h;
    options.worker_threads = 4;
    options.max_worker_threads = 32;
    return options;
}

struct Harness {
    explicit Harness(neomcp::HttpServerOptions options = quick_options(), std::size_t calls = 4)
        : server(neomcp::ServerOptions{.name = "endpoint-test",
                                       .version = "0.0.1",
                                       .max_concurrent_calls = calls,
                                       .protocol_logging = true}) {
        server.tool<EchoArgs>("echo", "Return the text", [](const EchoArgs& args) { return args.text; });
        server.tool("progress", "Report progress three times, then answer", [](neomcp::Context& context) {
            context.progress(1, 3);
            context.progress(2, 3);
            context.progress(3, 3);
            return std::string("done");
        });
        server.tool("log", "Log three messages, then answer", [](neomcp::Context& context) {
            context.info("one");
            context.info("two");
            context.info("three");
            return std::string("logged");
        });
        // Waits for the test, with nothing written before the answer.
        server.tool("hold", "Wait for the test", [this](neomcp::Context& context) {
            started.add();
            const bool opened = gate.wait(context.stop_token());
            ended.push(opened);
            return std::string(opened ? "released" : "stopped");
        });
        // The same, after one progress notification (so the answer is an event stream).
        server.tool("hold_streaming", "Report once, then wait for the test", [this](neomcp::Context& context) {
            context.progress(1);
            started.add();
            const bool opened = gate.wait(context.stop_token());
            ended.push(opened);
            return std::string(opened ? "released" : "stopped");
        });
        // A notification too large for a small stream buffer, a small one, and a short answer.
        server.tool("noisy", "Report a large and a small notification", [](neomcp::Context& context) {
            context.progress(1, std::nullopt, std::string(4000, 'x'));
            context.progress(2);
            return std::string("short");
        });
        // A notification, then an answer too large for a small stream buffer.
        server.tool("large", "Report once, then answer at length", [](neomcp::Context& context) {
            context.progress(1);
            return std::string(4000, 'y');
        });
        http = std::make_unique<neomcp::HttpServer>(server, std::move(options));
        auto started_at = http->start();
        if (!started_at) throw std::runtime_error("start failed: " + started_at.error);
        port = *started_at.value;
    }

    Gate gate;
    Counter started;
    Mailbox<bool> ended;  // per call of hold: true when the test released it, false when it saw its stop
    neomcp::Server server;
    std::unique_ptr<neomcp::HttpServer> http;
    std::uint16_t port = 0;
};

// ---- The client ------------------------------------------------------------------------------------------------------

std::unique_ptr<httplib::Client> make_client(std::uint16_t port) {
    auto client = std::make_unique<httplib::Client>("127.0.0.1", port);
    client->set_keep_alive(false);
    client->set_connection_timeout(30, 0);
    client->set_read_timeout(60, 0);
    client->set_write_timeout(60, 0);
    return client;
}

struct Reply {
    bool ok = false;  // a complete response arrived
    int status = 0;
    httplib::Headers headers;
    std::string body;

    std::string header(const std::string& name) const {
        const auto found = headers.find(name);
        return found == headers.end() ? std::string() : found->second;
    }
    json message() const { return json::parse(body); }
};

// One request that runs on a thread of its own and is read as it arrives.
class Call {
public:
    Call(std::uint16_t port, std::string method, httplib::Headers headers, std::string body = {})
        : client_(make_client(port)) {
        request_.method = std::move(method);
        request_.path = path;
        request_.headers = std::move(headers);
        request_.body = std::move(body);
        request_.response_handler = [this](const httplib::Response& response) {
            {
                std::lock_guard lock(mutex_);
                reply_.status = response.status;
                reply_.headers = response.headers;
                have_headers_ = true;
            }
            changed_.notify_all();
            return true;
        };
        request_.content_receiver = [this](const char* data, std::size_t length, std::size_t, std::size_t) {
            {
                std::lock_guard lock(mutex_);
                reply_.body.append(data, length);
            }
            changed_.notify_all();
            return true;
        };
        thread_ = std::thread([this] {
            const auto result = client_->send(request_);
            {
                std::lock_guard lock(mutex_);
                reply_.ok = static_cast<bool>(result);
                if (result && !have_headers_) {
                    reply_.status = result->status;
                    reply_.headers = result->headers;
                    reply_.body = result->body;
                }
                finished_ = true;
            }
            changed_.notify_all();
        });
    }
    Call(const Call&) = delete;
    Call& operator=(const Call&) = delete;
    ~Call() {
        client_->stop();
        if (thread_.joinable()) thread_.join();
    }

    bool wait_for_headers() {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, patience, [&] { return have_headers_ || finished_; });
        return have_headers_;
    }
    bool wait_until(const std::function<bool(const std::string&)>& predicate,
                    std::chrono::milliseconds timeout = patience) {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, timeout, [&] { return predicate(reply_.body) || finished_; });
        return predicate(reply_.body);
    }
    // Blocks until the request ended and returns what came.
    Reply finish(std::chrono::milliseconds timeout = patience) {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, timeout, [&] { return finished_; });
        return reply_;
    }
    bool finished() {
        std::lock_guard lock(mutex_);
        return finished_;
    }
    Reply now() {
        std::lock_guard lock(mutex_);
        return reply_;
    }
    // Closes the connection from the client's side.
    void disconnect() { client_->stop(); }

private:
    std::unique_ptr<httplib::Client> client_;
    httplib::Request request_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable changed_;
    Reply reply_;
    bool have_headers_ = false;
    bool finished_ = false;
};

Reply send(std::uint16_t port, const std::string& method, httplib::Headers headers, std::string body = {}) {
    Call call(port, method, std::move(headers), std::move(body));
    return call.finish();
}

// The messages of an event stream body, in order; comments are skipped.
std::vector<json> events(const std::string& body) {
    std::vector<json> out;
    std::size_t start = 0;
    while (start < body.size()) {
        std::size_t end = body.find("\n\n", start);
        if (end == std::string::npos) break;  // an event not yet complete
        std::string data;
        std::size_t line = start;
        while (line < end) {
            std::size_t stop = body.find('\n', line);
            if (stop == std::string::npos || stop > end) stop = end;
            const std::string text = body.substr(line, stop - line);
            if (text.rfind("data:", 0) == 0) {
                std::string value = text.substr(5);
                if (!value.empty() && value.front() == ' ') value.erase(0, 1);
                if (!data.empty()) data += '\n';
                data += value;
            }
            line = stop + 1;
        }
        if (!data.empty()) out.push_back(json::parse(data));
        start = end + 2;
    }
    return out;
}

bool has_answer(const std::string& body) {
    for (const json& message : events(body)) {
        if (message.contains("result") || message.contains("error")) return true;
    }
    return false;
}

std::string rpc(const json& id, const std::string& method, json params = json::object()) {
    json message{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};
    return message.dump();
}

std::string notification(const std::string& method) {
    return json{{"jsonrpc", "2.0"}, {"method", method}}.dump();
}

// The _meta of a request that names its revision, as the runner writes it.
json modern_meta(const char* revision = modern, json capabilities = json::object()) {
    return json{{"io.modelcontextprotocol/protocolVersion", revision},
                {"io.modelcontextprotocol/clientInfo", {{"name", "conformance-test-client"}, {"version", "1.0.0"}}},
                {"io.modelcontextprotocol/clientCapabilities", std::move(capabilities)}};
}

// Adds the members of `meta` to the params' _meta (which may hold a progress token already).
json with_meta(json params, const json& meta = modern_meta()) {
    if (!params.contains("_meta")) params["_meta"] = json::object();
    for (auto member = meta.begin(); member != meta.end(); ++member) params["_meta"][member.key()] = member.value();
    return params;
}

// The headers of a request that names its revision.
httplib::Headers modern_headers(const std::string& method, std::optional<std::string> name = std::nullopt,
                                const char* revision = modern, const char* accept = both_types) {
    httplib::Headers headers{{"Content-Type", json_type},
                             {"Accept", accept},
                             {"MCP-Protocol-Version", revision},
                             {"Mcp-Method", method}};
    if (name) headers.emplace("Mcp-Name", *name);
    return headers;
}

Reply modern_call(std::uint16_t port, const json& id, const std::string& method, json params = json::object(),
                  std::optional<std::string> name = std::nullopt) {
    return send(port, "POST", modern_headers(method, name), rpc(id, method, with_meta(std::move(params))));
}

json initialize_params(const char* revision = legacy) {
    return json{{"protocolVersion", revision},
                {"capabilities", json::object()},
                {"clientInfo", {{"name", "conformance-test-client"}, {"version", "1.0.0"}}}};
}

httplib::Headers legacy_headers(const std::string& session, const char* accept = both_types) {
    httplib::Headers headers{{"Content-Type", json_type}, {"Accept", accept}, {"MCP-Protocol-Version", legacy}};
    if (!session.empty()) headers.emplace("Mcp-Session-Id", session);
    return headers;
}

// A legacy session: initialize, then notifications/initialized. Returns the session id.
std::string open_session(std::uint16_t port) {
    const Reply opened = send(port, "POST", {{"Content-Type", json_type}, {"Accept", both_types}},
                              rpc(1, "initialize", initialize_params()));
    if (opened.status != 200) throw std::runtime_error("initialize answered " + std::to_string(opened.status));
    const std::string id = opened.header("Mcp-Session-Id");
    if (id.empty()) throw std::runtime_error("initialize gave no session id");
    const Reply ready = send(port, "POST", legacy_headers(id), notification("notifications/initialized"));
    if (ready.status != 202) throw std::runtime_error("initialized answered " + std::to_string(ready.status));
    return id;
}

Reply legacy_call(std::uint16_t port, const std::string& session, const json& id, const std::string& method,
                  json params = json::object()) {
    return send(port, "POST", legacy_headers(session), rpc(id, method, std::move(params)));
}

json call_params(const std::string& tool, json arguments = json::object()) {
    return json{{"name", tool}, {"arguments", std::move(arguments)}};
}

json with_progress(json params, const json& token) {
    if (!params.contains("_meta")) params["_meta"] = json::object();
    params["_meta"]["progressToken"] = token;
    return params;
}

int code_of(const json& message) {
    return message.contains("error") ? message.at("error").at("code").get<int>() : 0;
}

// ---- The timer -------------------------------------------------------------------------------------------------------

TEST(HttpEndpointTimer, RunsJobsInTheOrderOfTheirTimesAndDropsCancelledOnes) {
    neomcp::http::detail::Timer timer;
    Mailbox<int> ran;
    const auto now = neomcp::http::detail::Timer::Clock::now();
    timer.schedule(now + 60ms, [&] { ran.push(3); });
    timer.schedule(now + 20ms, [&] { ran.push(1); });
    const auto dropped = timer.schedule(now + 30ms, [&] { ran.push(99); });
    timer.schedule(now + 40ms, [&] { ran.push(2); });
    timer.cancel(dropped);
    EXPECT_EQ(ran.pop(), 1);
    EXPECT_EQ(ran.pop(), 2);
    EXPECT_EQ(ran.pop(), 3);
    EXPECT_FALSE(ran.pop(100ms).has_value());
    timer.stop();
    EXPECT_EQ(timer.schedule(now, [&] { ran.push(4); }), 0U);
}

// ---- Revision 2026-07-28: requests that name their revision ----------------------------------------------------------

TEST(HttpEndpointModern, RefusesAMetaThatLacksWhatTheRevisionRequires) {
    Harness harness;
    const json without_version{{"io.modelcontextprotocol/clientCapabilities", json::object()}};
    const json without_capabilities{{"io.modelcontextprotocol/protocolVersion", modern}};
    const std::vector<std::string> bodies{
        rpc(11, "server/discover", json::object()),
        rpc(12, "server/discover", json{{"_meta", without_version}}),
        rpc(13, "server/discover", json{{"_meta", without_capabilities}}),
    };
    int id = 11;
    for (const std::string& body : bodies) {
        const Reply reply = send(harness.port, "POST", modern_headers("server/discover"), body);
        EXPECT_EQ(reply.status, 400) << body;
        EXPECT_EQ(reply.header("Content-Type"), json_type);
        const json message = reply.message();
        EXPECT_EQ(code_of(message), -32602) << body;
        EXPECT_EQ(message.at("id"), id++) << "an error answers the request's id";
    }
}

TEST(HttpEndpointModern, DiscoverAnswersWithoutClientInfoAndNamesTheServer) {
    Harness harness;
    json meta = modern_meta();
    meta.erase("io.modelcontextprotocol/clientInfo");
    const Reply reply =
        send(harness.port, "POST", modern_headers("server/discover"), rpc(201, "server/discover", with_meta({}, meta)));
    ASSERT_EQ(reply.status, 200);
    EXPECT_EQ(reply.header("Content-Type"), json_type);
    EXPECT_TRUE(reply.header("Mcp-Session-Id").empty()) << "a request that names its revision gets no session";
    const json result = reply.message().at("result");
    EXPECT_TRUE(result.at("supportedVersions").is_array());
    EXPECT_TRUE(result.at("capabilities").contains("tools"));
    EXPECT_FALSE(result.at("capabilities").contains("prompts"));
    const json& info = result.at("_meta").at("io.modelcontextprotocol/serverInfo");
    EXPECT_EQ(info.at("name"), "endpoint-test");
    EXPECT_TRUE(info.contains("version"));
    EXPECT_EQ(harness.http->session_count(), 0U);
}

TEST(HttpEndpointModern, ListsToolsAndAnswersAnUnservedMethodWith404) {
    Harness harness;
    const Reply tools = modern_call(harness.port, 2, "tools/list");
    ASSERT_EQ(tools.status, 200);
    EXPECT_TRUE(tools.message().at("result").at("tools").is_array());

    const Reply prompts = modern_call(harness.port, 3, "prompts/list");
    EXPECT_EQ(prompts.status, 404);
    EXPECT_EQ(code_of(prompts.message()), -32601);

    // Methods the revision removed, and one no revision has.
    int id = 500;
    for (const char* method :
         {"initialize", "ping", "logging/setLevel", "resources/subscribe", "resources/unsubscribe", "unknown/method"}) {
        const Reply reply = modern_call(harness.port, id, method);
        EXPECT_EQ(reply.status, 404) << method;
        const json message = reply.message();
        EXPECT_EQ(code_of(message), -32601) << method;
        EXPECT_EQ(message.at("id"), id) << method;
        ++id;
    }
}

TEST(HttpEndpointModern, AnUnsupportedVersionIs400WithTheSupportedList) {
    Harness harness;
    const Reply discover = modern_call(harness.port, 1, "server/discover");
    ASSERT_EQ(discover.status, 200);
    const json supported = discover.message().at("result").at("supportedVersions");

    const Reply reply = send(harness.port, "POST", modern_headers("server/discover", std::nullopt, "v999.0.0"),
                             rpc(622, "server/discover", with_meta({}, modern_meta("v999.0.0"))));
    EXPECT_EQ(reply.status, 400);
    const json message = reply.message();
    EXPECT_EQ(message.at("id"), 622);
    const json& data = message.at("error").at("data");
    EXPECT_EQ(data.at("requested"), "v999.0.0");
    ASSERT_FALSE(data.at("supported").empty());
    for (const json& version : data.at("supported")) {
        EXPECT_NE(std::find(supported.begin(), supported.end(), version), supported.end()) << version.dump();
    }
}

TEST(HttpEndpointModern, AHeaderThatContradictsTheBodyIs400HeaderMismatchBeforeTheVersion) {
    Harness harness;
    const Reply reply = send(harness.port, "POST", modern_headers("server/discover"),
                             rpc(686, "server/discover", with_meta({}, modern_meta("v999.0.0"))));
    EXPECT_EQ(reply.status, 400);
    EXPECT_EQ(code_of(reply.message()), -32020);

    httplib::Headers no_version{{"Content-Type", json_type}, {"Accept", both_types}, {"Mcp-Method", "server/discover"}};
    const Reply absent = send(harness.port, "POST", no_version, rpc(687, "server/discover", with_meta({})));
    EXPECT_EQ(absent.status, 400);
    EXPECT_EQ(code_of(absent.message()), -32020);
}

TEST(HttpEndpointModern, NoLogMessageWithoutALogLevelAndLogsWithOne) {
    Harness harness;
    const Reply quiet = modern_call(harness.port, 943, "tools/call", call_params("log"), "log");
    ASSERT_EQ(quiet.status, 200);
    EXPECT_EQ(quiet.header("Content-Type"), json_type) << "nothing came before the answer";
    EXPECT_EQ(quiet.message().at("result").at("content").at(0).at("text"), "logged");

    json meta = modern_meta();
    meta["io.modelcontextprotocol/logLevel"] = "info";
    const Reply loud = send(harness.port, "POST", modern_headers("tools/call", "log"),
                            rpc(944, "tools/call", with_meta(call_params("log"), meta)));
    ASSERT_EQ(loud.status, 200);
    EXPECT_EQ(loud.header("Content-Type"), "text/event-stream");
    const std::vector<json> frames = events(loud.body);
    ASSERT_EQ(frames.size(), 4U);
    for (int i = 0; i < 3; ++i) EXPECT_EQ(frames[i].at("method"), "notifications/message");
    EXPECT_EQ(frames[3].at("id"), 944);
}

TEST(HttpEndpointModern, ProgressArrivesOnAnEventStreamBeforeTheAnswer) {
    Harness harness;
    const Reply reply = modern_call(harness.port, 7, "tools/call", with_progress(call_params("progress"), "p-1"),
                                    "progress");
    ASSERT_EQ(reply.status, 200);
    EXPECT_EQ(reply.header("Content-Type"), "text/event-stream");
    EXPECT_EQ(reply.header("Cache-Control"), "no-store");
    EXPECT_EQ(reply.header("X-Accel-Buffering"), "no");
    const std::vector<json> frames = events(reply.body);
    ASSERT_EQ(frames.size(), 4U);
    double last = 0;
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(frames[i].at("method"), "notifications/progress");
        EXPECT_EQ(frames[i].at("params").at("progressToken"), "p-1");
        EXPECT_GT(frames[i].at("params").at("progress").get<double>(), last);
        last = frames[i].at("params").at("progress").get<double>();
    }
    EXPECT_EQ(frames[3].at("id"), 7);
    EXPECT_EQ(frames[3].at("result").at("content").at(0).at("text"), "done");
}

TEST(HttpEndpointModern, AClientThatTakesOnlyJsonGetsTheAnswerWithoutTheNotifications) {
    Harness harness;
    const Reply reply =
        send(harness.port, "POST", modern_headers("tools/call", "progress", modern, json_type),
             rpc(8, "tools/call", with_meta(with_progress(call_params("progress"), "p-2"))));
    ASSERT_EQ(reply.status, 200);
    EXPECT_EQ(reply.header("Content-Type"), json_type);
    EXPECT_EQ(reply.message().at("id"), 8);
}

TEST(HttpEndpointModern, AClientThatTakesOnlyEventStreamsGetsTheAnswerAsAnEvent) {
    Harness harness;
    const Reply reply = send(harness.port, "POST", modern_headers("tools/list", std::nullopt, modern, "text/event-stream"),
                             rpc(9, "tools/list", with_meta({})));
    ASSERT_EQ(reply.status, 200);
    EXPECT_EQ(reply.header("Content-Type"), "text/event-stream");
    const std::vector<json> frames = events(reply.body);
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].at("id"), 9);
}

TEST(HttpEndpointModern, ThreeConcurrentPostsAreAllAnswered) {
    Harness harness;
    std::vector<std::unique_ptr<Call>> calls;
    for (int id = 1000; id < 1003; ++id) {
        calls.push_back(std::make_unique<Call>(
            harness.port, "POST", modern_headers("tools/list", std::nullopt, modern, "text/event-stream, application/json"),
            rpc(id, "tools/list", with_meta({}))));
    }
    int id = 1000;
    for (auto& call : calls) {
        const Reply reply = call->finish();
        EXPECT_EQ(reply.status, 200);
        EXPECT_EQ(reply.message().at("id"), id++);
    }
}

// ---- Revision 2025-11-25: sessions -----------------------------------------------------------------------------------

TEST(HttpEndpointLegacy, InitializeInEachHeaderVariantOpensASessionWithAVisibleAsciiId) {
    Harness harness;
    const std::vector<httplib::Headers> variants{
        {{"Content-Type", json_type}, {"Accept", both_types}},
        {{"Content-Type", json_type}, {"Accept", both_types}, {"mcp-protocol-version", legacy}},
        {{"Content-Type", json_type}, {"Accept", both_types}, {"mcp-protocol-version", legacy}, {"Mcp-Method", "initialize"}},
    };
    std::set<std::string> ids;
    for (const httplib::Headers& headers : variants) {
        const Reply reply = send(harness.port, "POST", headers, rpc(1, "initialize", initialize_params()));
        ASSERT_EQ(reply.status, 200);
        EXPECT_EQ(reply.header("Content-Type"), json_type);
        EXPECT_EQ(reply.message().at("result").at("protocolVersion"), legacy);
        const std::string id = reply.header("Mcp-Session-Id");
        EXPECT_EQ(id.size(), 64U) << "32 random bytes in hexadecimal";
        for (const char c : id) EXPECT_TRUE(c >= 0x21 && c <= 0x7e);
        ids.insert(id);
    }
    EXPECT_EQ(ids.size(), 3U);
    EXPECT_EQ(harness.http->session_count(), 3U);
}

TEST(HttpEndpointLegacy, ASessionCallsATool_ThenDeleteEndsIt_AndItsIdIsUnknownAfterwards) {
    Harness harness;
    const std::string session = open_session(harness.port);

    const Reply call = legacy_call(harness.port, session, 2, "tools/call", call_params("echo", {{"text", "hi"}}));
    ASSERT_EQ(call.status, 200);
    EXPECT_EQ(call.message().at("result").at("content").at(0).at("text"), "hi");
    EXPECT_TRUE(call.header("Mcp-Session-Id").empty()) << "the id is sent once, with the handshake's answer";

    const Reply ping = legacy_call(harness.port, session, 3, "ping");
    ASSERT_EQ(ping.status, 200);
    EXPECT_EQ(ping.message().at("result"), json::object());

    const Reply ended = send(harness.port, "DELETE", legacy_headers(session));
    EXPECT_EQ(ended.status, 204);
    EXPECT_EQ(harness.http->session_count(), 0U);

    const Reply after = legacy_call(harness.port, session, 4, "tools/list");
    EXPECT_EQ(after.status, 404);
    EXPECT_EQ(send(harness.port, "DELETE", legacy_headers(session)).status, 404);
    EXPECT_EQ(send(harness.port, "GET", legacy_headers(session, "text/event-stream")).status, 404);
}

TEST(HttpEndpointLegacy, ARequestWithoutASessionIs400AndAnUnknownIdIs404) {
    Harness harness;
    const Reply missing = legacy_call(harness.port, "", 2, "tools/list");
    EXPECT_EQ(missing.status, 400);
    EXPECT_EQ(missing.message().at("id"), 2);

    const Reply unknown = legacy_call(harness.port, std::string(64, 'a'), 3, "tools/list");
    EXPECT_EQ(unknown.status, 404);
    EXPECT_FALSE(unknown.message().contains("id")) << "the endpoint refused before reading any message";

    const Reply repeated = send(harness.port, "POST",
                                {{"Content-Type", json_type},
                                 {"Accept", both_types},
                                 {"Mcp-Session-Id", "a"},
                                 {"mcp-session-id", "b"}},
                                rpc(4, "tools/list"));
    EXPECT_EQ(repeated.status, 400);

    const Reply stray = send(harness.port, "POST", legacy_headers(""), notification("notifications/initialized"));
    EXPECT_EQ(stray.status, 400) << "a notification outside a session is refused";
}

TEST(HttpEndpointLegacy, ProgressArrivesOnAnEventStreamBeforeTheAnswer) {
    Harness harness;
    const std::string session = open_session(harness.port);
    const Reply reply =
        legacy_call(harness.port, session, 5, "tools/call", with_progress(call_params("progress"), "lp-1"));
    ASSERT_EQ(reply.status, 200);
    EXPECT_EQ(reply.header("Content-Type"), "text/event-stream");
    const std::vector<json> frames = events(reply.body);
    ASSERT_EQ(frames.size(), 4U);
    for (int i = 0; i < 3; ++i) EXPECT_EQ(frames[i].at("params").at("progressToken"), "lp-1");
    EXPECT_EQ(frames[3].at("id"), 5);
}

TEST(HttpEndpointLegacy, LogMessagesFollowTheLevelTheClientSet) {
    Harness harness;
    const std::string session = open_session(harness.port);
    ASSERT_EQ(legacy_call(harness.port, session, 2, "logging/setLevel", {{"level", "debug"}}).status, 200);
    const Reply reply = legacy_call(harness.port, session, 3, "tools/call", call_params("log"));
    ASSERT_EQ(reply.status, 200);
    const std::vector<json> frames = events(reply.body);
    ASSERT_EQ(frames.size(), 4U);
    EXPECT_EQ(frames[0].at("method"), "notifications/message");
}

TEST(HttpEndpointLegacy, ThreeConcurrentPostsOfOneSessionAreAllAnswered) {
    Harness harness;
    const std::string session = open_session(harness.port);
    std::vector<std::unique_ptr<Call>> calls;
    for (int id = 1000; id < 1003; ++id) {
        calls.push_back(std::make_unique<Call>(harness.port, "POST",
                                               legacy_headers(session, "text/event-stream, application/json"),
                                               rpc(id, "tools/list")));
    }
    int id = 1000;
    for (auto& call : calls) {
        const Reply reply = call->finish();
        EXPECT_EQ(reply.status, 200);
        EXPECT_EQ(reply.message().at("id"), id++);
    }
}

TEST(HttpEndpointLegacy, TheConnectionStreamOpensOnceAndEndsWithTheSession) {
    neomcp::HttpServerOptions options = quick_options();
    options.keep_alive_interval = 20ms;
    Harness harness(options);
    const std::string session = open_session(harness.port);
    Call stream(harness.port, "GET", legacy_headers(session, "text/event-stream"));
    ASSERT_TRUE(stream.wait_for_headers());
    EXPECT_EQ(stream.now().status, 200);
    EXPECT_EQ(stream.now().header("Content-Type"), "text/event-stream");
    EXPECT_TRUE(stream.wait_until([](const std::string& body) { return body.find(": ") != std::string::npos; }))
        << "an idle stream gets keep-alive comments";

    EXPECT_EQ(send(harness.port, "GET", legacy_headers(session, "text/event-stream")).status, 409);
    EXPECT_EQ(send(harness.port, "GET", legacy_headers(session, json_type)).status, 406);
    EXPECT_EQ(send(harness.port, "GET", legacy_headers("", "text/event-stream")).status, 405);

    EXPECT_EQ(send(harness.port, "DELETE", legacy_headers(session)).status, 204);
    const Reply ended = stream.finish();
    EXPECT_TRUE(ended.ok) << "the stream ends normally with its session";
}

TEST(HttpEndpointLegacy, AVersionHeaderThatNamesNoServedRevisionIsRefused) {
    Harness harness;
    const std::string session = open_session(harness.port);
    httplib::Headers headers = legacy_headers(session);
    headers.erase("MCP-Protocol-Version");
    headers.emplace("MCP-Protocol-Version", "1999-01-01");
    const Reply reply = send(harness.port, "POST", headers, rpc(2, "tools/list"));
    EXPECT_EQ(reply.status, 400);
    EXPECT_EQ(reply.message().at("id"), 2);
    EXPECT_EQ(send(harness.port, "DELETE", headers).status, 400);
    EXPECT_EQ(harness.http->session_count(), 1U) << "a refused DELETE ends nothing";
}

// ---- Host and Origin -------------------------------------------------------------------------------------------------

TEST(HttpEndpointRebinding, AForeignHostOrOriginIsRefusedInBothEras) {
    Harness harness;
    const std::string local = "127.0.0.1:" + std::to_string(harness.port);

    httplib::Headers modern_evil = modern_headers("server/discover");
    modern_evil.emplace("Host", "evil.example.com");
    modern_evil.emplace("Origin", "http://evil.example.com");
    const Reply refused = send(harness.port, "POST", modern_evil, rpc(1, "server/discover", with_meta({})));
    EXPECT_EQ(refused.status, 403);

    httplib::Headers modern_good = modern_headers("server/discover");
    modern_good.emplace("Host", local);
    modern_good.emplace("Origin", "http://" + local);
    EXPECT_EQ(send(harness.port, "POST", modern_good, rpc(2, "server/discover", with_meta({}))).status, 200);

    httplib::Headers legacy_evil{{"Content-Type", json_type},
                                 {"Accept", both_types},
                                 {"MCP-Protocol-Version", legacy},
                                 {"Host", "evil.example.com"},
                                 {"Origin", "http://evil.example.com"}};
    EXPECT_EQ(send(harness.port, "POST", legacy_evil, rpc(1, "initialize", initialize_params())).status, 403);
    EXPECT_EQ(harness.http->session_count(), 0U) << "a refused request opens nothing";

    httplib::Headers legacy_good{{"Content-Type", json_type},
                                 {"Accept", both_types},
                                 {"MCP-Protocol-Version", legacy},
                                 {"Host", "localhost:" + std::to_string(harness.port)},
                                 {"Origin", "http://localhost:" + std::to_string(harness.port)}};
    const Reply opened = send(harness.port, "POST", legacy_good, rpc(1, "initialize", initialize_params()));
    ASSERT_EQ(opened.status, 200);
    httplib::Headers initialized = legacy_good;
    initialized.emplace("Mcp-Session-Id", opened.header("Mcp-Session-Id"));
    EXPECT_EQ(send(harness.port, "POST", initialized, notification("notifications/initialized")).status, 202);

    httplib::Headers other_origin = modern_headers("server/discover");
    other_origin.emplace("Origin", "http://evil.example.com");
    EXPECT_EQ(send(harness.port, "POST", other_origin, rpc(3, "server/discover", with_meta({}))).status, 403)
        << "a good Host does not excuse a foreign Origin";
}

TEST(HttpEndpointRebinding, StartFailsOnANonLoopbackAddressWithoutNamedHosts) {
    neomcp::Server server({.name = "s", .version = "1"});
    neomcp::HttpServerOptions options;
    options.bind_address = "0.0.0.0";
    neomcp::HttpServer http(server, options);
    const auto started = http.start();
    ASSERT_FALSE(started);
    EXPECT_NE(started.error.find("allowed_hosts"), std::string::npos) << started.error;
    EXPECT_EQ(http.port(), 0);
}

// ---- Request checks --------------------------------------------------------------------------------------------------

TEST(HttpEndpointChecks, MethodMediaTypeAcceptAndBodyBound) {
    neomcp::Server server({.name = "s", .version = "1", .max_message_bytes = 512});
    neomcp::HttpServer http(server, quick_options());
    const auto started = http.start();
    ASSERT_TRUE(started);
    const std::uint16_t port = *started.value;
    const std::string body = rpc(1, "tools/list", with_meta({}));

    const Reply put = send(port, "PUT", modern_headers("tools/list"), body);
    EXPECT_EQ(put.status, 405);
    EXPECT_EQ(put.header("Allow"), "POST, GET, DELETE");

    httplib::Headers text = modern_headers("tools/list");
    text.erase("Content-Type");
    text.emplace("Content-Type", "text/plain");
    EXPECT_EQ(send(port, "POST", text, body).status, 415);

    httplib::Headers charset = modern_headers("tools/list");
    charset.erase("Content-Type");
    charset.emplace("Content-Type", "application/json; charset=utf-8");
    EXPECT_EQ(send(port, "POST", charset, body).status, 200) << "parameters of the media type do not matter";

    httplib::Headers html = modern_headers("tools/list", std::nullopt, modern, "text/html");
    EXPECT_EQ(send(port, "POST", html, body).status, 406);
    // The HTTP library lets this through; the endpoint's parser does not.
    httplib::Headers malformed = modern_headers("tools/list", std::nullopt, modern, "*/json");
    EXPECT_EQ(send(port, "POST", malformed, body).status, 406);

    const std::string large = rpc(1, "tools/list", with_meta({{"padding", std::string(1000, 'p')}}));
    EXPECT_EQ(send(port, "POST", modern_headers("tools/list"), large).status, 413);

    EXPECT_EQ(send(port, "GET", modern_headers("tools/list")).status, 405) << "no stream without a session";
}

// ---- Abandoned requests ----------------------------------------------------------------------------------------------

TEST(HttpEndpointAbandon, AModernClientClosingItsStreamCancelsTheCall) {
    Harness harness;
    Call call(harness.port, "POST", modern_headers("tools/call", "hold_streaming"),
              rpc(1, "tools/call", with_meta(with_progress(call_params("hold_streaming"), "t"))));
    ASSERT_TRUE(call.wait_until([](const std::string& body) { return !events(body).empty(); }));
    ASSERT_TRUE(harness.started.wait_for_at_least(1));
    call.disconnect();
    const auto ended = harness.ended.pop();
    ASSERT_TRUE(ended.has_value());
    EXPECT_FALSE(*ended) << "the handler saw its stop";
}

TEST(HttpEndpointAbandon, AModernClientLeavingBeforeAJsonAnswerCancelsTheCall) {
    Harness harness;
    Call call(harness.port, "POST", modern_headers("tools/call", "hold"),
              rpc(1, "tools/call", with_meta(call_params("hold"))));
    ASSERT_TRUE(harness.started.wait_for_at_least(1));
    call.disconnect();
    const auto ended = harness.ended.pop();
    ASSERT_TRUE(ended.has_value());
    EXPECT_FALSE(*ended) << "the disconnect reached the handler as a stop";
}

TEST(HttpEndpointAbandon, ALegacyClientsDisconnectDoesNotCancelTheCall) {
    Harness harness;
    const std::string session = open_session(harness.port);
    {
        Call streamed(harness.port, "POST", legacy_headers(session),
                      rpc(2, "tools/call", with_progress(call_params("hold_streaming"), "t")));
        ASSERT_TRUE(streamed.wait_until([](const std::string& body) { return !events(body).empty(); }));
        Call plain(harness.port, "POST", legacy_headers(session), rpc(3, "tools/call", call_params("hold")));
        ASSERT_TRUE(harness.started.wait_for_at_least(2));
        streamed.disconnect();
        plain.disconnect();
        // The revision says a disconnect is not a cancellation; a window of many poll intervals shows none came.
        EXPECT_FALSE(harness.ended.pop(300ms).has_value());
    }
    harness.gate.open();
    for (int i = 0; i < 2; ++i) {
        const auto ended = harness.ended.pop();
        ASSERT_TRUE(ended.has_value());
        EXPECT_TRUE(*ended) << "the calls ran to their end";
    }
    // The session goes on.
    EXPECT_EQ(legacy_call(harness.port, session, 4, "tools/list").status, 200);
}

TEST(HttpEndpointAbandon, ALegacyCancellationNotificationEndsTheRequestWithoutAnAnswer) {
    Harness harness;
    const std::string session = open_session(harness.port);
    Call call(harness.port, "POST", legacy_headers(session), rpc(7, "tools/call", call_params("hold")));
    ASSERT_TRUE(harness.started.wait_for_at_least(1));
    const json cancel{{"jsonrpc", "2.0"},
                      {"method", "notifications/cancelled"},
                      {"params", {{"requestId", 7}, {"reason", "test"}}}};
    EXPECT_EQ(send(harness.port, "POST", legacy_headers(session), cancel.dump()).status, 202);
    const Reply reply = call.finish();
    EXPECT_EQ(reply.status, 202) << "the request was taken and will not be answered";
    const auto ended = harness.ended.pop();
    ASSERT_TRUE(ended.has_value());
    EXPECT_FALSE(*ended);
}

// ---- Response form ---------------------------------------------------------------------------------------------------

TEST(HttpEndpointForm, ASilentRequestGetsAStreamAfterStreamAfterWithKeepAliveComments) {
    neomcp::HttpServerOptions options = quick_options();
    options.endpoint.stream_after = 50ms;
    options.keep_alive_interval = 30ms;
    Harness harness(options);
    Call call(harness.port, "POST", modern_headers("tools/call", "hold"),
              rpc(1, "tools/call", with_meta(call_params("hold"))));
    ASSERT_TRUE(call.wait_for_headers());
    EXPECT_EQ(call.now().status, 200);
    EXPECT_EQ(call.now().header("Content-Type"), "text/event-stream");
    EXPECT_TRUE(call.wait_until([](const std::string& body) { return body.find(": ") != std::string::npos; }));
    harness.gate.open();
    const Reply reply = call.finish();
    ASSERT_TRUE(reply.ok);
    const std::vector<json> frames = events(reply.body);
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].at("result").at("content").at(0).at("text"), "released");
}

TEST(HttpEndpointForm, AJsonOnlyClientWaitsForItsAnswerWhateverStreamAfterSays) {
    neomcp::HttpServerOptions options = quick_options();
    options.endpoint.stream_after = 20ms;
    Harness harness(options);
    Call call(harness.port, "POST", modern_headers("tools/call", "hold", modern, json_type),
              rpc(1, "tools/call", with_meta(call_params("hold"))));
    ASSERT_TRUE(harness.started.wait_for_at_least(1));
    // No stream may start; a window of several stream_after periods shows none did.
    EXPECT_FALSE(call.wait_until([](const std::string& body) { return !body.empty(); }, 150ms));
    harness.gate.open();
    const Reply reply = call.finish();
    EXPECT_EQ(reply.status, 200);
    EXPECT_EQ(reply.header("Content-Type"), json_type);
}

TEST(HttpEndpointForm, ANotificationTooLargeForTheStreamIsDroppedAndTheAnswerArrives) {
    neomcp::HttpServerOptions options = quick_options();
    options.endpoint.max_stream_buffer_bytes = 512;
    Harness harness(options);
    const Reply reply =
        modern_call(harness.port, 1, "tools/call", with_progress(call_params("noisy"), "n"), "noisy");
    ASSERT_TRUE(reply.ok);
    const std::vector<json> frames = events(reply.body);
    ASSERT_EQ(frames.size(), 2U);
    EXPECT_EQ(frames[0].at("params").at("progress"), 2);
    EXPECT_EQ(frames[1].at("result").at("content").at(0).at("text"), "short");
}

TEST(HttpEndpointForm, AnAnswerTooLargeForTheStreamEndsTheStreamInsteadOfWaiting) {
    neomcp::HttpServerOptions options = quick_options();
    options.endpoint.max_stream_buffer_bytes = 512;
    Harness harness(options);
    const Reply reply =
        modern_call(harness.port, 1, "tools/call", with_progress(call_params("large"), "l"), "large");
    EXPECT_EQ(reply.status, 200);
    EXPECT_FALSE(reply.ok) << "the stream was cut, not ended as if it were complete";
    EXPECT_FALSE(has_answer(reply.body));
    EXPECT_EQ(events(reply.body).size(), 1U) << "the notification before the answer arrived";
    // The endpoint is still fine.
    EXPECT_EQ(modern_call(harness.port, 2, "tools/list").status, 200);
}

// ---- Sessions: bound, idle time, stop --------------------------------------------------------------------------------

TEST(HttpEndpointSessions, AHandshakeBeyondTheBoundIs503) {
    neomcp::HttpServerOptions options = quick_options();
    options.endpoint.max_sessions = 2;
    Harness harness(options);
    const std::string first = open_session(harness.port);
    open_session(harness.port);
    const Reply refused = send(harness.port, "POST", {{"Content-Type", json_type}, {"Accept", both_types}},
                               rpc(1, "initialize", initialize_params()));
    EXPECT_EQ(refused.status, 503);
    EXPECT_TRUE(refused.header("Mcp-Session-Id").empty());
    EXPECT_EQ(harness.http->session_count(), 2U);
    // Requests that need no session still work.
    EXPECT_EQ(modern_call(harness.port, 2, "tools/list").status, 200);
    EXPECT_EQ(send(harness.port, "DELETE", legacy_headers(first)).status, 204);
    open_session(harness.port);
}

// Waits until the endpoint keeps `count` sessions; false when that did not happen in time.
bool wait_for_sessions(neomcp::HttpServer& http, std::size_t count) {
    const auto start = std::chrono::steady_clock::now();
    while (since(start) < patience) {
        if (http.session_count() == count) return true;
        std::this_thread::sleep_for(5ms);  // pacing a look at a counter; not the mechanism under test
    }
    return false;
}

TEST(HttpEndpointSessions, AnIdleSessionEndsAndAnOpenStreamKeepsItAlive) {
    neomcp::HttpServerOptions options = quick_options();
    // Long enough for the GET to arrive on a loaded machine before the session it names is idle that long.
    options.endpoint.session_idle_timeout = 500ms;
    Harness harness(options);
    const std::string kept = open_session(harness.port);
    {
        Call stream(harness.port, "GET", legacy_headers(kept, "text/event-stream"));
        ASSERT_TRUE(stream.wait_for_headers());
        ASSERT_EQ(stream.now().status, 200);
        const std::string idle = open_session(harness.port);
        ASSERT_TRUE(wait_for_sessions(*harness.http, 1));  // the one without a stream ended
        EXPECT_EQ(legacy_call(harness.port, idle, 2, "tools/list").status, 404);
        EXPECT_EQ(legacy_call(harness.port, kept, 3, "tools/list").status, 200);
        stream.disconnect();
    }
    ASSERT_TRUE(wait_for_sessions(*harness.http, 0));
    EXPECT_EQ(legacy_call(harness.port, kept, 4, "tools/list").status, 404);
}

TEST(HttpEndpointSessions, StopClosesSessionsAndStreamsAndAnswersWhatWaits) {
    Harness harness;
    const std::string session = open_session(harness.port);
    Call stream(harness.port, "GET", legacy_headers(session, "text/event-stream"));
    ASSERT_TRUE(stream.wait_for_headers());
    Call waiting(harness.port, "POST", legacy_headers(session), rpc(2, "tools/call", call_params("hold")));
    Call streaming(harness.port, "POST", modern_headers("tools/call", "hold_streaming"),
                   rpc(3, "tools/call", with_meta(with_progress(call_params("hold_streaming"), "s"))));
    ASSERT_TRUE(harness.started.wait_for_at_least(2));
    ASSERT_TRUE(streaming.wait_until([](const std::string& body) { return !events(body).empty(); }));

    const auto start = std::chrono::steady_clock::now();
    harness.http->stop();
    std::cout << "[measure] stop() with a session, a stream and two calls took " << since(start).count() << " ms\n";
    EXPECT_LT(since(start), promptly);
    EXPECT_EQ(waiting.finish().status, 503);
    EXPECT_FALSE(has_answer(streaming.finish().body));
    EXPECT_TRUE(stream.finish().status == 200);
    EXPECT_EQ(harness.http->session_count(), 0U);
    EXPECT_EQ(harness.http->port(), 0);
    harness.gate.open();
}

// ---- Races -----------------------------------------------------------------------------------------------------------

TEST(HttpEndpointRace, AnAnswerRacingTheClientsDisconnectLeavesTheSessionUsable) {
    Harness harness(quick_options(), 8);
    const std::string session = open_session(harness.port);
    harness.gate.open();  // every hold returns at once: its answer races the disconnect
    constexpr int rounds = 40;
    for (int round = 0; round < rounds; ++round) {
        // Half of them in the modern era, outside the session.
        const bool modern_round = round % 2 == 1;
        Call call(harness.port, "POST",
                  modern_round ? modern_headers("tools/call", "hold") : legacy_headers(session),
                  rpc(100 + round, "tools/call",
                      modern_round ? with_meta(call_params("hold")) : call_params("hold")));
        ASSERT_TRUE(harness.started.wait_for_at_least(round + 1));  // the call runs: now its answer races
        call.disconnect();
    }
    for (int round = 0; round < rounds; ++round) ASSERT_TRUE(harness.ended.pop().has_value());
    EXPECT_EQ(legacy_call(harness.port, session, 2, "tools/call", call_params("echo", {{"text", "still"}}))
                  .message()
                  .at("result")
                  .at("content")
                  .at(0)
                  .at("text"),
              "still");
    EXPECT_EQ(send(harness.port, "DELETE", legacy_headers(session)).status, 204);
}

TEST(HttpEndpointRace, ADeleteRacingARequestOfTheSameSessionAnswersBoth) {
    Harness harness(quick_options(), 8);
    for (int round = 0; round < 20; ++round) {
        const std::string session = open_session(harness.port);
        Call call(harness.port, "POST", legacy_headers(session), rpc(1, "tools/call", call_params("echo", {{"text", "x"}})));
        Call ending(harness.port, "DELETE", legacy_headers(session));
        const Reply answer = call.finish();
        const Reply ended = ending.finish();
        EXPECT_TRUE(answer.status == 200 || answer.status == 404) << answer.status;
        EXPECT_EQ(ended.status, 204);
        EXPECT_EQ(legacy_call(harness.port, session, 2, "tools/list").status, 404);
    }
    EXPECT_EQ(harness.http->session_count(), 0U);
}

TEST(HttpEndpointRace, ADeleteWhileACallWaitsAnswersTheCallAsEnded) {
    Harness harness;
    const std::string session = open_session(harness.port);
    Call call(harness.port, "POST", legacy_headers(session), rpc(1, "tools/call", call_params("hold")));
    ASSERT_TRUE(harness.started.wait_for_at_least(1));
    EXPECT_EQ(send(harness.port, "DELETE", legacy_headers(session)).status, 204);
    EXPECT_EQ(call.finish().status, 404);
    const auto ended = harness.ended.pop();
    ASSERT_TRUE(ended.has_value());
    EXPECT_FALSE(*ended) << "closing the session stops its calls";
}

TEST(HttpEndpointRace, StopRacingRequestsAnswersEveryOneAndReturnsInTime) {
    Harness harness(quick_options(), 8);
    const std::string session = open_session(harness.port);
    std::atomic<bool> go{true};
    std::vector<std::thread> clients;
    Counter answered;
    for (int i = 0; i < 6; ++i) {
        clients.emplace_back([&, i] {
            int id = 1000 * i;
            while (go.load()) {
                const Reply reply = i % 2 == 0 ? legacy_call(harness.port, session, ++id, "tools/list")
                                               : modern_call(harness.port, ++id, "tools/list");
                if (!reply.ok) break;  // the listener stopped
                EXPECT_TRUE(reply.status == 200 || reply.status == 404 || reply.status == 503) << reply.status;
                answered.add();
            }
        });
    }
    ASSERT_TRUE(answered.wait_for_at_least(30));
    const auto start = std::chrono::steady_clock::now();
    harness.http->stop();
    EXPECT_LT(since(start), promptly);
    go = false;
    for (auto& client : clients) client.join();
}

TEST(HttpEndpointRace, RequestsOfOneSessionCompletingOnDifferentThreadsGetTheirOwnAnswers) {
    Harness harness(quick_options(), 8);
    const std::string session = open_session(harness.port);
    for (int round = 0; round < 5; ++round) {
        std::vector<std::unique_ptr<Call>> calls;
        for (int i = 0; i < 8; ++i) {
            const std::string text = "r" + std::to_string(round) + "c" + std::to_string(i);
            calls.push_back(std::make_unique<Call>(harness.port, "POST", legacy_headers(session),
                                                   rpc(round * 10 + i, "tools/call", call_params("echo", {{"text", text}}))));
        }
        for (int i = 0; i < 8; ++i) {
            const Reply reply = calls[static_cast<std::size_t>(i)]->finish();
            ASSERT_EQ(reply.status, 200);
            const json message = reply.message();
            EXPECT_EQ(message.at("id"), round * 10 + i);
            EXPECT_EQ(message.at("result").at("content").at(0).at("text"),
                      "r" + std::to_string(round) + "c" + std::to_string(i));
        }
    }
}

// ---- The endpoint without HttpServer --------------------------------------------------------------------------------

TEST(HttpEndpointHandler, AnswersWithoutAnyListenerThroughItsHandler) {
    neomcp::Server server({.name = "s", .version = "1"});
    auto policy = neomcp::http::HostPolicy::create({});
    ASSERT_TRUE(policy);
    auto endpoint = neomcp::http::Endpoint::create(server, std::move(*policy.value));
    ASSERT_TRUE(endpoint);
    neomcp::http::Request request;
    request.method = "POST";
    request.path = path;
    request.headers = {{"Host", "localhost"},
                       {"Content-Type", json_type},
                       {"Accept", both_types},
                       {"MCP-Protocol-Version", modern},
                       {"Mcp-Method", "tools/list"}};
    request.body = rpc(1, "tools/list", with_meta({}));
    std::promise<neomcp::http::Response> answer;
    auto got = answer.get_future();
    (*endpoint.value)->handle(std::move(request), neomcp::http::Responder([&](neomcp::http::Response&& response) {
        answer.set_value(std::move(response));
        return true;
    }));
    ASSERT_EQ(got.wait_for(patience), std::future_status::ready);
    const neomcp::http::Response response = got.get();
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(json::parse(response.body).at("id"), 1);
    (*endpoint.value)->stop();
}

TEST(HttpEndpointHandler, ABodyBeyondTheServersMessageBoundIs413AndStoppingRefusesWith503) {
    neomcp::Server server({.name = "s", .version = "1", .max_message_bytes = 64});
    auto policy = neomcp::http::HostPolicy::create({});
    ASSERT_TRUE(policy);
    auto endpoint = neomcp::http::Endpoint::create(server, std::move(*policy.value));
    ASSERT_TRUE(endpoint);
    const auto answer = [&](std::string body) {
        neomcp::http::Request request;
        request.method = "POST";
        request.headers = {{"Host", "localhost"}, {"Content-Type", json_type}};
        request.body = std::move(body);
        int status = 0;
        (*endpoint.value)->handle(std::move(request), neomcp::http::Responder([&](neomcp::http::Response&& response) {
            status = response.status;
            return true;
        }));
        return status;  // these refusals are completed before handle() returns
    };
    EXPECT_EQ(answer(std::string(65, ' ')), 413);
    (*endpoint.value)->stop();
    EXPECT_EQ(answer("{}"), 503);
}

TEST(HttpEndpointHandler, CreateRefusesOptionsOutOfRange) {
    neomcp::Server server({.name = "s", .version = "1"});
    for (int which = 0; which < 4; ++which) {
        neomcp::http::EndpointOptions options;
        if (which == 0) options.max_sessions = 0;
        if (which == 1) options.session_idle_timeout = 0ms;
        if (which == 2) options.stream_after = 0ms;
        if (which == 3) options.max_stream_buffer_bytes = 0;
        auto policy = neomcp::http::HostPolicy::create({});
        ASSERT_TRUE(policy);
        EXPECT_FALSE(neomcp::http::Endpoint::create(server, std::move(*policy.value), options)) << which;
    }
}

}  // namespace
