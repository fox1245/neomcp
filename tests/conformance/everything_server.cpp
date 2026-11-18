// The server the official conformance runner tests: Streamable HTTP on 127.0.0.1, path /mcp, both eras on the one
// endpoint, with the tools the runner's tool scenarios call.
//
//   neomcp_conformance_fixture [port]
//
// The port defaults to 0, which picks a free one; the chosen port is then written to standard error on a line of its
// own before anything else, so that a script can read it. The server runs until SIGINT or SIGTERM.
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>

#include "neomcp/http_server.h"
#include "neomcp/server.h"

namespace {

// A PNG of one pixel, and a WAV header without samples, in base64.
constexpr const char* pixel_png =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8/5+hHgAHggJ/PchI7wAAAABJRU5ErkJggg==";
constexpr const char* silent_wav = "UklGRiQAAABXQVZFZm10IBAAAAABAAEAQB8AAIA+AAACABAAZGF0YQAAAAA=";

neomcp::ImageContent image() {
    neomcp::ImageContent block;
    block.data = pixel_png;
    block.mime_type = "image/png";
    return block;
}

neomcp::EmbeddedResource embedded() {
    neomcp::EmbeddedResource block;
    block.resource = neomcp::TextResourceContents{"test://embedded-resource", "text/plain",
                                                  "This is an embedded resource content.", std::nullopt};
    return block;
}

neomcp::TextContent text(std::string value) {
    neomcp::TextContent block;
    block.text = std::move(value);
    return block;
}

bool read_port(const char* argument, std::uint16_t& port) {
    const char* end = argument + std::strlen(argument);
    const auto [stop, error] = std::from_chars(argument, end, port);
    return error == std::errc() && stop == end;
}

}  // namespace

int main(int argc, char** argv) {
    std::uint16_t port = 0;
    if (argc > 2 || (argc == 2 && !read_port(argv[1], port))) {
        std::fputs("usage: neomcp_conformance_fixture [port]\n", stderr);
        return 2;
    }

    neomcp::Server server({.name = "neomcp-conformance-fixture", .version = "0.0.1", .protocol_logging = true});

    server.tool("test_simple_text", "Return one text block",
                [] { return std::string("This is a simple text response for testing."); });

    server.tool("test_image_content", "Return one image block", [] {
        neomcp::ToolResult result;
        result.content = {image()};
        return result;
    });

    server.tool("test_audio_content", "Return one audio block", [] {
        neomcp::AudioContent audio;
        audio.data = silent_wav;
        audio.mime_type = "audio/wav";
        neomcp::ToolResult result;
        result.content = {audio};
        return result;
    });

    server.tool("test_embedded_resource", "Return one embedded resource", [] {
        neomcp::ToolResult result;
        result.content = {embedded()};
        return result;
    });

    server.tool("test_multiple_content_types", "Return a text, an image and an embedded resource", [] {
        neomcp::ToolResult result;
        result.content = {text("Multiple content types test:"), image(), embedded()};
        return result;
    });

    server.tool("test_error_handling", "Fail with a tool execution error", []() -> std::string {
        throw neomcp::ToolError("This tool intentionally returns an error for testing");
    });

    server.tool("test_tool_with_progress", "Report progress three times, then answer", [](neomcp::Context& context) {
        context.progress(0, 100);
        context.progress(50, 100);
        context.progress(100, 100);
        return std::string("Progress test completed");
    });

    // Log messages go out only where the client asked for them: a level set on the connection, or the request's own.
    const auto logging = [](neomcp::Context& context) {
        context.info("Tool execution started");
        context.info("Tool processing data");
        context.info("Tool execution completed");
        return std::string("Logging test completed");
    };
    server.tool("test_tool_with_logging", "Log three messages, then answer", logging);
    server.tool("test_logging_tool", "Log three messages, then answer", logging);

    neomcp::HttpServerOptions options;
    options.port = port;
    options.diagnostics = false;
    const bool announce = port == 0;
    options.on_listening = [announce](std::uint16_t bound) {
        if (!announce) return;
        std::fprintf(stderr, "%u\n", static_cast<unsigned>(bound));
        std::fflush(stderr);
    };
    const int code = neomcp::run_http(server, options);
    if (code != 0) std::fputs("neomcp_conformance_fixture: the server could not start\n", stderr);
    return code;
}
