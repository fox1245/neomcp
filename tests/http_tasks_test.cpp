// Tasks over the Streamable HTTP endpoint, revision 2026-07-28 with the extension io.modelcontextprotocol/tasks, over
// real sockets on the loopback interface. Each request there has a session of its own that ends with the request, so a
// task lives on after the request that created it, and is reached by later requests that name it in Mcp-Name.
//
// Every wait is bounded; a bound is reached only when something is broken.
#include <httplib.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>

#include <gtest/gtest.h>

#include "neomcp/http_server.h"
#include "neomcp/server.h"

namespace {

using namespace std::chrono_literals;
using neomcp::json;
using neomcp::TaskSupport;

constexpr std::chrono::milliseconds patience = 30s;
constexpr std::chrono::milliseconds poll_step = 5ms;
constexpr const char* modern = "2026-07-28";
constexpr const char* extension = "io.modelcontextprotocol/tasks";
constexpr const char* json_type = "application/json";
constexpr const char* both_types = "application/json, text/event-stream";
constexpr int missing_capability_code = -32021;
constexpr int header_mismatch_code = -32020;

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

json declaring(json more = json::object()) {
    more["extensions"][extension] = json::object();
    return more;
}

struct Reply {
    int status = 0;
    json message;  // the answer, from a JSON body or from the event stream
};

// The answer among the messages of an event stream body.
json answer_in_events(const std::string& body) {
    std::size_t start = 0;
    while (start < body.size()) {
        std::size_t end = body.find("\n\n", start);
        if (end == std::string::npos) end = body.size();
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
        if (!data.empty()) {
            json message = json::parse(data);
            if (message.contains("result") || message.contains("error")) return message;
        }
        start = end + 2;
    }
    return json();
}

class Rig {
public:
    Rig() : server(make_options()) {}

    void start() {
        neomcp::HttpServerOptions options;
        options.disconnect_poll_interval = 20ms;
        options.endpoint.stream_after = 1h;
        options.keep_alive_interval = 1h;
        options.worker_threads = 4;
        options.max_worker_threads = 32;
        http = std::make_unique<neomcp::HttpServer>(server, std::move(options));
        auto started = http->start();
        if (!started) throw std::runtime_error("start failed: " + started.error);
        port = *started.value;
    }

    // A POST of one request that names its revision, declaring `capabilities`, with Mcp-Name when `name` is given.
    Reply post(int id, const std::string& method, json params, const json& capabilities = declaring(),
               std::optional<std::string> name = std::nullopt) {
        params["_meta"] = json{{"io.modelcontextprotocol/protocolVersion", modern},
                               {"io.modelcontextprotocol/clientInfo", {{"name", "tasks-test"}, {"version", "1.0.0"}}},
                               {"io.modelcontextprotocol/clientCapabilities", capabilities}};
        const json body{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};
        httplib::Headers headers{{"Accept", both_types}, {"MCP-Protocol-Version", modern}, {"Mcp-Method", method}};
        if (name) headers.emplace("Mcp-Name", *name);
        httplib::Client client("127.0.0.1", port);
        client.set_keep_alive(false);
        client.set_connection_timeout(30, 0);
        client.set_read_timeout(60, 0);
        auto response = client.Post("/mcp", headers, body.dump(), json_type);
        if (!response) throw std::runtime_error("no response to " + method);
        Reply reply;
        reply.status = response->status;
        const std::string type = response->get_header_value("Content-Type");
        if (type.rfind("text/event-stream", 0) == 0) {
            reply.message = answer_in_events(response->body);
        } else if (!response->body.empty()) {
            reply.message = json::parse(response->body);
        }
        return reply;
    }

    Reply call(int id, const std::string& tool, const json& capabilities = declaring()) {
        return post(id, "tools/call", json{{"name", tool}, {"arguments", json::object()}}, capabilities, tool);
    }

    Reply get(int id, const std::string& task) { return post(id, "tasks/get", json{{"taskId", task}}, declaring(), task); }

    // tasks/get until the status is `wanted` (or, when empty, a terminal one); the last answer.
    json poll_until(const std::string& task, const std::string& wanted = {}) {
        const auto until = std::chrono::steady_clock::now() + patience;
        int id = 1000;
        json answer;
        while (std::chrono::steady_clock::now() < until) {
            answer = get(++id, task).message;
            if (!answer.contains("result")) return answer;
            const std::string status = answer["result"].value("status", "");
            const bool ended = status == "completed" || status == "failed" || status == "cancelled";
            if (wanted.empty() ? ended : status == wanted) return answer;
            std::this_thread::sleep_for(poll_step);
        }
        return answer;
    }

    neomcp::Server server;
    std::unique_ptr<neomcp::HttpServer> http;
    std::uint16_t port = 0;

private:
    static neomcp::ServerOptions make_options() {
        neomcp::ServerOptions options{.name = "tasks-http", .version = "1.0.0"};
        options.tasks.store = neomcp::make_memory_task_store();
        options.tasks.min_poll_interval = 1ms;
        return options;
    }
};

neomcp::ElicitationForm name_form() {
    return neomcp::ElicitationForm{
        "Name?", json::parse(R"({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})")};
}

using Outcome = std::variant<std::string, neomcp::InputRequired>;

}  // namespace

// A3.2: the request that created a task, and its session, end with its answer; the task's handler runs on, and its
// stop token is not requested by that end.
TEST(ServerTasksWireHttpTest, ATaskOutlivesTheRequestThatCreatedIt) {
    Event started;
    Event release;
    Rig rig;
    rig.server.tool("hold", "Wait for the test", [&](neomcp::Context& call) {
        started.set();
        release.wait();
        return std::string(call.cancelled() ? "stopped" : "released");
    }).task_support(TaskSupport::optional);
    rig.start();

    const Reply created = rig.call(1, "hold");
    ASSERT_EQ(created.status, 200) << created.message.dump();
    ASSERT_EQ(created.message["result"]["resultType"], "task") << created.message.dump();
    const std::string task = created.message["result"]["taskId"].get<std::string>();
    ASSERT_TRUE(started.wait());
    const Reply working = rig.get(2, task);
    EXPECT_EQ(working.message["result"]["status"], "working") << working.message.dump();
    release.set();
    const json done = rig.poll_until(task);
    EXPECT_EQ(done["result"]["status"], "completed") << done.dump();
    EXPECT_EQ(done["result"]["result"]["content"][0]["text"], "released");
}

// The capabilities of the request that created a task stay with the task: it asks after that request's session ended,
// and the answers come with tasks/update.
TEST(ServerTasksWireHttpTest, ATaskAsksForInputAfterItsSessionEnded) {
    Event go;
    Rig rig;
    rig.server.tool("ask_later", "Ask once the test says so", [&](neomcp::Context& call) -> Outcome {
        const auto found = call.input_responses().find("name");
        if (found != call.input_responses().end()) {
            const auto* answer = std::get_if<neomcp::ElicitationAnswer>(&found->second);
            return "Hello, " + (answer != nullptr ? answer->content.value("name", std::string()) : std::string());
        }
        go.wait();
        if (!call.client_supports(name_form())) return std::string("not asked");
        neomcp::InputRequired asked;
        asked.requests.emplace("name", name_form());
        return asked;
    }).task_support(TaskSupport::optional);
    rig.start();

    const Reply created = rig.call(1, "ask_later", declaring(json{{"elicitation", json::object()}}));
    ASSERT_EQ(created.message["result"]["resultType"], "task") << created.message.dump();
    const std::string task = created.message["result"]["taskId"].get<std::string>();
    go.set();
    const json waiting = rig.poll_until(task, "input_required");
    ASSERT_EQ(waiting["result"]["status"], "input_required") << waiting.dump();
    ASSERT_TRUE(waiting["result"]["inputRequests"].contains("1.name")) << waiting.dump();
    const Reply ack = rig.post(2, "tasks/update",
                               json{{"taskId", task},
                                    {"inputResponses", {{"1.name", {{"action", "accept"}, {"content", {{"name", "Ada"}}}}}}}},
                               declaring(), task);
    EXPECT_EQ(ack.status, 200);
    EXPECT_EQ(ack.message["result"]["resultType"], "complete") << ack.message.dump();
    const json done = rig.poll_until(task);
    EXPECT_EQ(done["result"]["result"]["content"][0]["text"], "Hello, Ada") << done.dump();
}

// C4.10: tasks/get, tasks/update and tasks/cancel carry the task's id in Mcp-Name; a missing or different one is
// refused as every header that contradicts the body is.
TEST(ServerTasksWireHttpTest, TheRoutingHeaderMustNameTheTask) {
    Rig rig;
    rig.server.tool("quick", "Answer", [] { return std::string("done"); }).task_support(TaskSupport::optional);
    rig.start();
    const std::string task = rig.call(1, "quick").message["result"]["taskId"].get<std::string>();
    int id = 2;
    for (const std::string method : {"tasks/get", "tasks/cancel"}) {
        const Reply other = rig.post(id++, method, json{{"taskId", task}}, declaring(), std::string("another-task"));
        EXPECT_EQ(other.status, 400) << method;
        EXPECT_EQ(other.message["error"]["code"], header_mismatch_code) << method << " " << other.message.dump();
        const Reply missing = rig.post(id++, method, json{{"taskId", task}}, declaring(), std::nullopt);
        EXPECT_EQ(missing.status, 400) << method;
        EXPECT_EQ(missing.message["error"]["code"], header_mismatch_code) << method << " " << missing.message.dump();
    }
    const Reply named = rig.get(id++, task);
    EXPECT_EQ(named.status, 200);
    EXPECT_TRUE(named.message.contains("result")) << named.message.dump();
}

// C4.6 over HTTP: a cancellation that names the task is acknowledged, and the task is cancelled.
TEST(ServerTasksWireHttpTest, ACancellationThatNamesTheTaskIsAcknowledged) {
    Event started;
    Rig rig;
    rig.server.tool("long", "Run until cancelled", [&](neomcp::Context& call) {
        started.set();
        std::mutex mutex;
        std::condition_variable woken;
        const std::stop_callback wake(call.stop_token(), [&] {
            const std::lock_guard<std::mutex> lock(mutex);
            woken.notify_all();
        });
        std::unique_lock<std::mutex> lock(mutex);
        woken.wait_for(lock, patience, [&] { return call.cancelled(); });
        return std::string("stopped");
    }).task_support(TaskSupport::optional);
    rig.start();
    const std::string task = rig.call(1, "long").message["result"]["taskId"].get<std::string>();
    ASSERT_TRUE(started.wait());
    const Reply ack = rig.post(2, "tasks/cancel", json{{"taskId", task}}, declaring(), task);
    EXPECT_EQ(ack.status, 200);
    EXPECT_EQ(ack.message["result"]["resultType"], "complete") << ack.message.dump();
    EXPECT_EQ(rig.poll_until(task)["result"]["status"], "cancelled");
}

// C2.6 over HTTP: the refusal of a required tool for a request that does not declare the extension is a 400.
TEST(ServerTasksWireHttpTest, ARequiredToolIsRefusedWithTheStatusOfAMissingCapability) {
    Rig rig;
    rig.server.tool("always", "Only a task", [] { return std::string("done"); }).task_support(TaskSupport::required);
    rig.start();
    const Reply refused = rig.call(1, "always", json::object());
    EXPECT_EQ(refused.status, 400);
    EXPECT_EQ(refused.message["error"]["code"], missing_capability_code) << refused.message.dump();
    const Reply undeclared = rig.post(2, "tasks/get", json{{"taskId", "x"}}, json::object(), std::string("x"));
    EXPECT_EQ(undeclared.status, 400);
    EXPECT_EQ(undeclared.message["error"]["code"], missing_capability_code) << undeclared.message.dump();
}
