// What the server the official conformance runner tests serves: the tools, resources, resource template, prompts and
// completer that the runner's scenarios call and read, and the options of the server. A program that serves them over
// Streamable HTTP is everything_server.cpp; a test that sends the scenarios' requests to the same registrations is
// fixture_test.cpp.
//
// Nothing here depends on a protocol revision: the server registers what it offers, and the library answers for
// each revision as that revision prescribes.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "neomcp/server.h"

namespace conformance {

// The arguments of the prompt that takes two, and of the one that embeds a resource.
struct TwoArguments {
    std::string arg1;
    std::string arg2;
};

struct ResourceArgument {
    std::string resourceUri;
};

// The variable of the resource template.
struct TemplateVariable {
    std::string id;
};

}  // namespace conformance

NEOMCP_DESCRIBE(conformance::TwoArguments, arg1, "the first argument", arg2, "the second argument")
NEOMCP_DESCRIBE(conformance::ResourceArgument, resourceUri, "the URI of the resource to embed")
NEOMCP_DESCRIBE(conformance::TemplateVariable, id, "the identifier")

namespace conformance {

// A PNG of one pixel, and a WAV header without samples, in base64.
inline constexpr const char* pixel_png =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8/5+hHgAHggJ/PchI7wAAAABJRU5ErkJggg==";
inline constexpr const char* silent_wav = "UklGRiQAAABXQVZFZm10IBAAAAABAAEAQB8AAIA+AAACABAAZGF0YQAAAAA=";

inline neomcp::ImageContent image() {
    neomcp::ImageContent block;
    block.data = pixel_png;
    block.mime_type = "image/png";
    return block;
}

inline neomcp::EmbeddedResource embedded(std::string uri) {
    neomcp::EmbeddedResource block;
    block.resource = neomcp::TextResourceContents{std::move(uri), "text/plain", "This is an embedded resource content.",
                                                  std::nullopt};
    return block;
}

inline neomcp::TextContent text(std::string value) {
    neomcp::TextContent block;
    block.text = std::move(value);
    return block;
}

inline neomcp::ServerOptions server_options() {
    return {.name = "neomcp-conformance-fixture", .version = "0.0.1", .protocol_logging = true};
}

inline void register_tools(neomcp::Server& server) {
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
        result.content = {embedded("test://embedded-resource")};
        return result;
    });

    server.tool("test_multiple_content_types", "Return a text, an image and an embedded resource", [] {
        neomcp::ToolResult result;
        result.content = {text("Multiple content types test:"), image(), embedded("test://embedded-resource")};
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
}

inline void register_resources(neomcp::Server& server) {
    // A resource at a fixed URI: a string is text, and takes the MIME type the resource declares.
    server.resource("test://static-text", "Static text",
                    [] { return std::string("This is the content of the static text resource."); })
        .description("A resource with a fixed text")
        .mime_type("text/plain");

    // A blob is built whole, with its Base64 text and its own MIME type.
    server.resource("test://static-binary", "Static binary", [] {
        neomcp::BlobResourceContents contents;
        contents.uri = "test://static-binary";
        contents.mime_type = "image/png";
        contents.blob = pixel_png;
        return neomcp::ResourceContents(contents);
    }).description("A resource with a fixed image").mime_type("image/png");

    // A family of resources: the variable of the URI template is a member of the parameters, and a described value
    // is answered as JSON text.
    server.resource_template<TemplateVariable>("test://template/{id}/data", "Template data",
                                               [](const TemplateVariable& variable) {
                                                   return neomcp::json{{"id", variable.id},
                                                                       {"templateTest", true},
                                                                       {"data", "Data for ID: " + variable.id}};
                                               })
        .description("Data for the resource whose identifier the URI holds");
}

inline void register_prompts(neomcp::Server& server) {
    server.prompt("test_simple_prompt", "A prompt without arguments",
                  [] { return std::string("This is a simple prompt for testing."); });

    // The arguments are the members of the struct; both are required. A completer suggests the values of the first
    // that begin with what was typed.
    server
        .prompt<TwoArguments>("test_prompt_with_arguments", "A prompt that takes two arguments",
                              [](const TwoArguments& arguments) {
                                  return "Prompt with arguments: arg1='" + arguments.arg1 + "', arg2='" +
                                         arguments.arg2 + "'";
                              })
        .complete("arg1", [](std::string_view typed) {
            std::vector<std::string> values;
            for (const char* candidate : {"test", "testing", "tested"}) {
                if (std::string_view(candidate).starts_with(typed)) values.emplace_back(candidate);
            }
            return values;
        });

    server.prompt<ResourceArgument>("test_prompt_with_embedded_resource", "A prompt that embeds a resource",
                                    [](const ResourceArgument& argument) {
                                        neomcp::PromptMessage message;
                                        message.content = embedded(argument.resourceUri);
                                        return message;
                                    });

    server.prompt("test_prompt_with_image", "A prompt that carries an image", [] {
        neomcp::PromptMessage message;
        message.content = image();
        return message;
    });
}

inline void register_everything(neomcp::Server& server) {
    register_tools(server);
    register_resources(server);
    register_prompts(server);
}

}  // namespace conformance
