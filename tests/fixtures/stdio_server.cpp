// A small MCP server over stdio. The interoperability test starts it as a subprocess and drives it with another
// implementation's client; it is also the shortest tour of the API: typed tools, results of every kind, failures,
// progress, cancellation, and a coroutine handler.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>

#include <asio/as_tuple.hpp>
#include <asio/awaitable.hpp>
#include <asio/error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include "neomcp/server.h"
#include "neomcp/stdio.h"

struct AddArgs {
    int a;
    int b;
    std::optional<std::string> note;
};
NEOMCP_DESCRIBE(AddArgs,
    a, "first addend",
    b, "second addend",
    note, "a note to echo back")

struct AddResult {
    int sum;
    std::optional<std::string> note;
};
NEOMCP_DESCRIBE(AddResult,
    sum, "the sum of the addends",
    note, "the note that came with the call")

struct EchoArgs {
    std::string text;
};
NEOMCP_DESCRIBE(EchoArgs, text, "the text to return")

struct CountArgs {
    int steps;
};
NEOMCP_DESCRIBE(CountArgs, steps, "how many steps to report")

struct WaitArgs {
    int milliseconds;
};
NEOMCP_DESCRIBE(WaitArgs, milliseconds, "how long to wait at most")

struct Stats {
    int waits_started;
    int waits_cancelled;
};
NEOMCP_DESCRIBE(Stats,
    waits_started, "calls of wait that began waiting",
    waits_cancelled, "calls of wait that were cancelled while waiting")

struct BoundedArgs {
    int count;
};

namespace {
std::atomic<int> waits_started{0};
std::atomic<int> waits_cancelled{0};
}  // namespace

// A field option written out: the count may not be negative.
template <>
struct neomcp::Described<BoundedArgs> {
    static constexpr auto fields =
        neomcp::fields(neomcp::field(&BoundedArgs::count, "count", "how many, at least zero", neomcp::minimum(0)));
};

int main() {
    neomcp::Server server({.name = "neomcp-stdio-fixture", .version = "0.0.1"});

    // Arguments and result are described structs: the schemas of both are derived from them.
    server.tool<AddArgs>("add", "Add two integers", [](const AddArgs& args) {
        return AddResult{args.a + args.b, args.note};
    });

    // A string result is one text block.
    server.tool<EchoArgs>("echo", "Return the text it was given", [](const EchoArgs& args) { return args.text; });

    // A result built by hand, with a block of every kind.
    server.tool("content", "Return one block of every content kind", [] {
        neomcp::ToolResult result;
        neomcp::TextContent text;
        text.text = "a text block";
        neomcp::ImageContent image;
        image.data = "aW1hZ2U=";
        image.mime_type = "image/png";
        neomcp::AudioContent audio;
        audio.data = "YXVkaW8=";
        audio.mime_type = "audio/wav";
        neomcp::ResourceLink link;
        link.uri = "file:///fixture/link.txt";
        link.name = "link.txt";
        neomcp::EmbeddedResource embedded;
        embedded.resource = neomcp::TextResourceContents{"file:///fixture/embedded.txt", "text/plain", "embedded text", std::nullopt};
        result.content = {text, image, audio, link, embedded};
        return result;
    });

    // Failures. ToolError is a result with isError and its message; any other exception is one with a fixed message.
    server.tool("fail", "Fail with a tool error", []() -> std::string { throw neomcp::ToolError("the fixture fails on purpose"); });
    server.tool("crash", "Throw an exception that is not a tool error",
                []() -> std::string { throw std::runtime_error("a detail that must stay on the server"); });

    // Progress, and a handler that looks at its stop token between steps.
    server.tool<CountArgs>("count", "Report progress in steps", [](const CountArgs& args, neomcp::Context& context) {
        int done = 0;
        for (; done < args.steps && !context.stop_token().stop_requested(); ++done) {
            context.progress(done + 1, args.steps, "step " + std::to_string(done + 1));
        }
        return "counted " + std::to_string(done);
    });

    // A coroutine handler: it waits on a timer, which the server cancels when the client cancels the call.
    server.tool<WaitArgs>("wait", "Wait for a time, or until the call is cancelled",
                          [](WaitArgs args, neomcp::Context& context) -> asio::awaitable<std::string> {
        const auto executor = co_await asio::this_coro::executor;
        asio::steady_timer timer(executor, std::chrono::milliseconds(args.milliseconds));
        ++waits_started;
        context.progress(0, std::nullopt, "waiting");
        const auto outcome = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        if (std::get<0>(outcome) == asio::error::operation_aborted) ++waits_cancelled;
        co_return std::string("waited");
    });

    server.tool("stats", "Report how many calls of wait began and were cancelled", [] {
        return Stats{waits_started.load(), waits_cancelled.load()};
    });

    // A field option: arguments that fail it never reach the handler.
    server.tool<BoundedArgs>("bounded", "Accept a count of at least zero",
                             [](const BoundedArgs& args) { return "count " + std::to_string(args.count); });

    return neomcp::run_stdio(server);
}
