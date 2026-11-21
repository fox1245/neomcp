// Tasks over the Streamable HTTP endpoint, revision 2025-11-25, over real sockets on the loopback interface. The
// connection is a kept session; a tasks/result that waits holds its POST open, and the requests for input of the task
// arrive as events of that POST's stream, answered by POSTs of their own.
//
// Every wait is bounded; a bound is reached only when something is broken.
#include <httplib.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/http_server.h"
#include "neomcp/server.h"

namespace {

using namespace std::chrono_literals;
using neomcp::json;
using neomcp::TaskSupport;

constexpr std::chrono::milliseconds patience = 30s;
constexpr std::chrono::milliseconds poll_step = 5ms;
constexpr std::chrono::milliseconds quiet = 300ms;
constexpr const char* legacy = "2025-11-25";
constexpr const char* json_type = "application/json";
constexpr const char* both_types = "application/json, text/event-stream";
constexpr const char* session_header = "Mcp-Session-Id";
constexpr const char* related_task = "io.modelcontextprotocol/related-task";

class Event {
public:
    void set() {
        const std::lock_guard<std::mutex> lock(mutex_);
        set_ = true;
        changed_.notify_all();
    }
    bool wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, patience, [&] { return set_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool set_ = false;
};

// The messages of an event stream body, in order.
std::vector<json> events_in(const std::string& body) {
    std::vector<json> messages;
    std::size_t start = 0;
    while (start < body.size()) {
        const std::size_t end = body.find("\n\n", start);
        if (end == std::string::npos) break;  // an event not complete yet
        std::string data;
        std::size_t line = start;
        while (line < end) {
            std::size_t stop = body.find('\n', line);
            if (stop == std::string::npos || stop > end) stop = end;
            const std::string text = body.substr(line, stop - line);
            if (text.rfind("data:", 0) == 0) {
                std::string value = text.substr(5);
                if (!value.empty() && value.front() == ' ') value.erase(0, 1);
                data += value;
            }
            line = stop + 1;
        }
        if (!data.empty()) messages.push_back(json::parse(data));
        start = end + 2;
    }
    return messages;
}

// The first message of the body that satisfies `wanted`; null when none does.
json first_of(const std::string& body, const std::function<bool(const json&)>& wanted) {
    for (const json& message : events_in(body)) {
        if (wanted(message)) return message;
    }
    return json();
}

bool is_answer(const json& message) { return message.contains("result") || message.contains("error"); }
bool is_request(const json& message) { return message.contains("method") && message.contains("id"); }

// The body as messages: a JSON body is one message, an event stream as many as it has.
json answer_of(const httplib::Response& response) {
    if (response.get_header_value("Content-Type").rfind("text/event-stream", 0) == 0) {
        return first_of(response.body, is_answer);
    }
    return response.body.empty() ? json() : json::parse(response.body);
}

// One POST (or, without a body, a GET) that runs on a thread of its own and is read as it arrives.
class Call {
public:
    Call(std::uint16_t port, httplib::Headers headers, std::optional<std::string> body) : client_("127.0.0.1", port) {
        client_.set_keep_alive(false);
        client_.set_connection_timeout(30, 0);
        client_.set_read_timeout(60, 0);
        request_.method = body ? "POST" : "GET";
        request_.path = "/mcp";
        request_.headers = std::move(headers);
        if (body) {
            request_.headers.emplace("Content-Type", json_type);
            request_.body = std::move(*body);
        }
        request_.response_handler = [this](const httplib::Response& response) {
            {
                const std::lock_guard lock(mutex_);
                status_ = response.status;
                streamed_ = response.get_header_value("Content-Type").rfind("text/event-stream", 0) == 0;
                have_headers_ = true;
            }
            changed_.notify_all();
            return true;
        };
        request_.content_receiver = [this](const char* data, std::size_t length, std::size_t, std::size_t) {
            {
                const std::lock_guard lock(mutex_);
                body_.append(data, length);
            }
            changed_.notify_all();
            return true;
        };
        thread_ = std::thread([this] {
            const auto result = client_.send(request_);
            {
                const std::lock_guard lock(mutex_);
                ok_ = static_cast<bool>(result);
                finished_ = true;
            }
            changed_.notify_all();
        });
    }
    Call(const Call&) = delete;
    Call& operator=(const Call&) = delete;
    ~Call() {
        client_.stop();
        if (thread_.joinable()) thread_.join();
    }

    // The first message so far that satisfies `wanted` (of the event stream, or the JSON body once it is complete),
    // waiting for it; null after the bound.
    json wait_for(const std::function<bool(const json&)>& wanted, std::chrono::milliseconds bound = patience) {
        std::unique_lock lock(mutex_);
        json found;
        changed_.wait_for(lock, bound, [&] {
            found = look(wanted);
            return !found.is_null() || finished_;
        });
        if (found.is_null()) found = look(wanted);
        return found;
    }
    bool finished() {
        const std::lock_guard lock(mutex_);
        return finished_;
    }
    // The status, once the headers arrived; zero when they did not within the patience.
    int wait_for_headers() {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, patience, [&] { return have_headers_ || finished_; });
        return status_;
    }
    // Closes the connection from the client's side.
    void disconnect() { client_.stop(); }

private:
    // With mutex_ held.
    json look(const std::function<bool(const json&)>& wanted) const {
        if (streamed_) return first_of(body_, wanted);
        if (!finished_ || body_.empty()) return json();
        json whole = json::parse(body_);
        return wanted(whole) ? whole : json();
    }

    httplib::Client client_;
    httplib::Request request_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::string body_;
    bool ok_ = false;
    int status_ = 0;
    bool have_headers_ = false;
    bool streamed_ = false;
    bool finished_ = false;
};

class Rig {
public:
    explicit Rig(neomcp::ServerOptions options) : server(std::move(options)) {}

    void start() {
        neomcp::HttpServerOptions options;
        options.disconnect_poll_interval = 20ms;
        options.keep_alive_interval = 1h;
        // A request that waits turns into an event stream soon, so that its headers tell that it was taken.
        options.endpoint.stream_after = 50ms;
        options.worker_threads = 4;
        options.max_worker_threads = 32;
        http = std::make_unique<neomcp::HttpServer>(server, std::move(options));
        auto started = http->start();
        if (!started) throw std::runtime_error("start failed: " + started.error);
        port = *started.value;
    }

    httplib::Headers headers() const {
        httplib::Headers headers{{"Accept", both_types}};
        if (!session.empty()) {
            headers.emplace(session_header, session);
            headers.emplace("MCP-Protocol-Version", legacy);
        }
        return headers;
    }

    httplib::Result post(const json& body) {
        httplib::Client client("127.0.0.1", port);
        client.set_keep_alive(false);
        client.set_connection_timeout(30, 0);
        client.set_read_timeout(60, 0);
        auto response = client.Post("/mcp", headers(), body.dump(), json_type);
        if (!response) throw std::runtime_error("no response");
        return response;
    }
    json request(int id, const std::string& method, json params) {
        auto response = post(json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}});
        return answer_of(*response);
    }

    // Opens the kept session, with a client that can answer forms.
    void open() {
        auto response = post(json{{"jsonrpc", "2.0"},
                                  {"id", 0},
                                  {"method", "initialize"},
                                  {"params",
                                   {{"protocolVersion", legacy},
                                    {"capabilities", {{"elicitation", json::object()}}},
                                    {"clientInfo", {{"name", "tasks-2025-http"}, {"version", "1.0.0"}}}}}});
        ASSERT_EQ(response->status, 200) << response->body;
        session = response->get_header_value(session_header);
        ASSERT_FALSE(session.empty());
        json opened = answer_of(*response);
        ASSERT_TRUE(opened["result"]["capabilities"].contains("tasks")) << opened.dump();
        EXPECT_EQ(post(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}})->status, 202);
    }

    std::string start_task(int id, const std::string& tool) {
        json created =
            request(id, "tools/call", json{{"name", tool}, {"arguments", json::object()}, {"task", {{"ttl", 60000}}}});
        if (!created.contains("result")) throw std::runtime_error("no task: " + created.dump());
        return created["result"]["task"]["taskId"].get<std::string>();
    }

    std::string until(const std::string& task, const std::string& wanted) {
        const auto deadline = std::chrono::steady_clock::now() + patience;
        int id = 10000;
        std::string status;
        while (std::chrono::steady_clock::now() < deadline) {
            status = request(++id, "tasks/get", json{{"taskId", task}})["result"].value("status", "");
            if (status == wanted) return status;
            std::this_thread::sleep_for(poll_step);
        }
        return status;
    }

    Call stream() { return Call(port, headers(), std::nullopt); }

    Call result_call(int id, const std::string& task) {
        return Call(port, headers(),
                    json{{"jsonrpc", "2.0"}, {"id", id}, {"method", "tasks/result"}, {"params", {{"taskId", task}}}}
                        .dump());
    }

    neomcp::Server server;
    std::unique_ptr<neomcp::HttpServer> http;
    std::uint16_t port = 0;
    std::string session;
};

neomcp::ServerOptions options(std::size_t max_waiting_results = neomcp::TaskDefaults::max_waiting_results) {
    neomcp::ServerOptions options{.name = "tasks-2025-http", .version = "1.0.0"};
    options.tasks.store = neomcp::make_memory_task_store();
    options.tasks.min_poll_interval = 1ms;
    options.tasks.max_waiting_results = max_waiting_results;
    return options;
}

bool is_method(const json& message, const std::string& method) {
    return message.is_object() && message.value("method", std::string()) == method;
}

}  // namespace

// C2.1, C3.1, C3.2 over HTTP: a call asks for a task and is answered with it; tasks/get reads it; a tasks/result holds
// its POST open until the task ends and then carries the call's result, marked with the task.
TEST(ServerTasks2025HttpTest, AResultRequestHoldsItsPostUntilTheTaskEnds) {
    Rig rig(options());
    Event release;
    rig.server.tool("held", "Held until released", [&] {
        release.wait();
        return std::string("released");
    }).task_support(TaskSupport::optional);
    rig.server.tool("always", "Only a task", [] { return std::string("done"); }).task_support(TaskSupport::required);
    rig.start();
    rig.open();
    const std::string task = rig.start_task(1, "held");
    EXPECT_EQ(rig.request(2, "tasks/get", json{{"taskId", task}})["result"]["status"], "working");

    Call waiting = rig.result_call(3, task);
    EXPECT_TRUE(waiting.wait_for(is_answer, quiet).is_null()) << "answered before the task ended";
    EXPECT_FALSE(waiting.finished());
    release.set();
    json answer = waiting.wait_for(is_answer);
    ASSERT_TRUE(answer.contains("result")) << answer.dump();
    EXPECT_EQ(answer["result"]["content"][0]["text"], "released");
    EXPECT_EQ(answer["result"]["_meta"][related_task]["taskId"], task);

    json refused = rig.request(4, "tools/call", json{{"name", "always"}, {"arguments", json::object()}});
    EXPECT_EQ(refused["error"]["code"], neomcp::error_code::method_not_found) << refused.dump();
}

// C3.6 over HTTP: the request for input of a task arrives on the stream of the waiting tasks/result, marked with the
// task; its answer, POSTed on its own, reaches the task, and the stream ends with the task's result.
TEST(ServerTasks2025HttpTest, InputArrivesOnTheResultStream) {
    Rig rig(options());
    rig.server.tool("ask", "Ask a name", [](neomcp::Context& ctx) -> std::variant<std::string, neomcp::InputRequired> {
        const auto found = ctx.input_responses().find("name");
        if (found == ctx.input_responses().end()) {
            neomcp::InputRequired asked;
            asked.requests.emplace(
                "name", neomcp::ElicitationForm{"Who?", json::parse(R"({"type":"object","properties":{"name":{"type":"string"}}})")});
            return asked;
        }
        const auto* answer = std::get_if<neomcp::ElicitationAnswer>(&found->second);
        return "Hello, " + (answer != nullptr ? answer->content.value("name", std::string("?")) : std::string("?"));
    }).task_support(TaskSupport::required);
    rig.start();
    rig.open();
    const std::string task = rig.start_task(1, "ask");
    ASSERT_EQ(rig.until(task, "input_required"), "input_required");

    Call waiting = rig.result_call(2, task);
    json asked = waiting.wait_for(is_request);
    ASSERT_FALSE(asked.is_null()) << "no request for input on the stream";
    EXPECT_EQ(asked["method"], "elicitation/create");
    EXPECT_EQ(asked["params"]["_meta"][related_task]["taskId"], task) << asked.dump();
    auto answered = rig.post(json{{"jsonrpc", "2.0"},
                                  {"id", asked["id"]},
                                  {"result", {{"action", "accept"}, {"content", {{"name", "Ada"}}}}}});
    EXPECT_EQ(answered->status, 202) << answered->body;
    json answer = waiting.wait_for(is_answer);
    ASSERT_TRUE(answer.contains("result")) << answer.dump();
    EXPECT_EQ(answer["result"]["content"][0]["text"], "Hello, Ada");
    EXPECT_EQ(rig.until(task, "completed"), "completed");
}

// C1.1, C2.1, C3.3 over HTTP: the listing says which tools may run as tasks; a call that asks for a task of a tool that
// never runs as one is refused (-32601, an ordinary JSON-RPC error); a tool execution error fails the task, and
// tasks/result answers that result.
TEST(ServerTasks2025HttpTest, TheListingTheRefusalAndAToolErrorOverHttp) {
    Rig rig(options());
    rig.server.tool("tool_error", "Fail as a tool", []() -> std::string {
        throw neomcp::ToolError("the tool could not");
    }).task_support(TaskSupport::optional);
    rig.server.tool("never", "Never a task", [] { return std::string("plain"); });
    rig.start();
    rig.open();
    json listed = rig.request(1, "tools/list", json::object());
    ASSERT_EQ(listed["result"]["tools"].size(), 2u) << listed.dump();
    for (json tool : listed["result"]["tools"]) {
        if (tool["name"] == "tool_error") {
            EXPECT_EQ(tool["execution"]["taskSupport"], "optional") << tool.dump();
        } else {
            EXPECT_FALSE(tool.contains("execution")) << tool.dump();
        }
    }
    auto refused = rig.post(json{{"jsonrpc", "2.0"},
                                 {"id", 2},
                                 {"method", "tools/call"},
                                 {"params", {{"name", "never"}, {"arguments", json::object()}, {"task", json::object()}}}});
    EXPECT_EQ(refused->status, 200);
    EXPECT_EQ(answer_of(*refused)["error"]["code"], neomcp::error_code::method_not_found) << refused->body;

    const std::string task = rig.start_task(3, "tool_error");
    ASSERT_EQ(rig.until(task, "failed"), "failed");
    json result = rig.request(4, "tasks/result", json{{"taskId", task}});
    EXPECT_EQ(result["result"]["isError"], true) << result.dump();
    EXPECT_EQ(result["result"]["_meta"][related_task]["taskId"], task);
}

// C3.4 over HTTP: a cancellation POSTed while a tasks/result of the task waits on another POST answers with the
// cancelled task, and the waiting POST is answered.
TEST(ServerTasks2025HttpTest, ACancellationAnswersTheWaitingResultRequest) {
    Rig rig(options());
    rig.server.tool("long", "Run until cancelled", [](neomcp::Context& ctx) {
        while (!ctx.cancelled()) std::this_thread::sleep_for(poll_step);
        return std::string("finished anyway");
    }).task_support(TaskSupport::optional);
    rig.start();
    rig.open();
    const std::string task = rig.start_task(1, "long");
    Call waiting = rig.result_call(2, task);
    EXPECT_EQ(waiting.wait_for_headers(), 200);
    EXPECT_TRUE(waiting.wait_for(is_answer, quiet).is_null());
    json cancelled = rig.request(3, "tasks/cancel", json{{"taskId", task}});
    EXPECT_EQ(cancelled["result"]["status"], "cancelled") << cancelled.dump();
    json answer = waiting.wait_for(is_answer);
    EXPECT_TRUE(answer.contains("error")) << answer.dump();
    EXPECT_EQ(rig.until(task, "cancelled"), "cancelled");
}

// C3.2 over HTTP: a tasks/result whose client went away gives its place among the waiters back (the task runs on);
// while it holds it, one more is answered busy.
TEST(ServerTasks2025HttpTest, AResultRequestWhoseClientLeftGivesItsPlaceBack) {
    Rig rig(options(1));
    Event release;
    rig.server.tool("held", "Held until released", [&] {
        release.wait();
        return std::string("released");
    }).task_support(TaskSupport::optional);
    rig.start();
    rig.open();
    const std::string task = rig.start_task(1, "held");
    int id = 2;
    {
        Call first = rig.result_call(id++, task);
        ASSERT_EQ(first.wait_for_headers(), 200);
        Call second = rig.result_call(id++, task);
        json busy = second.wait_for(is_answer);
        EXPECT_EQ(busy["error"]["data"]["reason"], "busy") << busy.dump();
        first.disconnect();
    }
    // Once the endpoint saw the client leave, a new wait is taken.
    std::optional<Call> waiting;
    const auto deadline = std::chrono::steady_clock::now() + patience;
    while (std::chrono::steady_clock::now() < deadline) {
        waiting.emplace(rig.port, rig.headers(),
                        json{{"jsonrpc", "2.0"}, {"id", id++}, {"method", "tasks/result"}, {"params", {{"taskId", task}}}}
                            .dump());
        if (waiting->wait_for(is_answer, quiet).is_null()) break;
        waiting.reset();
    }
    ASSERT_TRUE(waiting) << "every later tasks/result was answered busy";
    release.set();
    json answer = waiting->wait_for(is_answer);
    EXPECT_EQ(answer["result"]["content"][0]["text"], "released") << answer.dump();
}

// C3.7, C3.8 over HTTP: the progress of a task's handler and the changes of its status belong to no request; they reach
// the session's GET stream.
TEST(ServerTasks2025HttpTest, ProgressAndStatusReachTheSessionStream) {
    Rig rig(options());
    rig.server.tool("report", "Report and end", [](neomcp::Context& ctx) {
        ctx.progress(0.5, 1.0);
        ctx.task_status("half way");
        return std::string("done");
    }).task_support(TaskSupport::optional);
    rig.start();
    rig.open();
    Call stream = rig.stream();
    ASSERT_EQ(stream.wait_for_headers(), 200);
    json created = rig.request(1, "tools/call",
                               json{{"name", "report"},
                                    {"arguments", json::object()},
                                    {"task", json::object()},
                                    {"_meta", {{"progressToken", "p-1"}}}});
    const std::string task = created["result"]["task"]["taskId"].get<std::string>();
    json progress = stream.wait_for([](const json& m) { return is_method(m, "notifications/progress"); });
    EXPECT_EQ(progress["params"]["progressToken"], "p-1") << progress.dump();
    json status = stream.wait_for([](const json& m) {
        return is_method(m, "notifications/tasks/status") && m.contains("params") &&
               m["params"].value("status", std::string()) == "completed";
    });
    EXPECT_EQ(status["params"]["taskId"], task) << status.dump();
}
