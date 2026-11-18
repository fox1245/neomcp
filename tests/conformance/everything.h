// What the server the official conformance runner tests serves: the tools, resources, resource template, prompts and
// completer that the runner's scenarios call and read, and the options of the server. A program that serves them over
// Streamable HTTP is everything_server.cpp; a test that sends the scenarios' requests to the same registrations is
// fixture_test.cpp.
//
// Nothing here depends on a protocol revision: the server registers what it offers, and the library answers for
// each revision as that revision prescribes.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
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

// The argument of the tool that asks the client's model, and of the one that asks the user.
struct PromptArgument {
    std::string prompt;
};

struct MessageArgument {
    std::string message;
};

}  // namespace conformance

NEOMCP_DESCRIBE(conformance::TwoArguments, arg1, "the first argument", arg2, "the second argument")
NEOMCP_DESCRIBE(conformance::ResourceArgument, resourceUri, "the URI of the resource to embed")
NEOMCP_DESCRIBE(conformance::TemplateVariable, id, "the identifier")
NEOMCP_DESCRIBE(conformance::PromptArgument, prompt, "the prompt to send to the client's model")
NEOMCP_DESCRIBE(conformance::MessageArgument, message, "the message to show the user")

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

// ---- Asking the client ----------------------------------------------------------------------------------------------
//
// A handler that needs something only the client can give returns what it asks for, and is entered again with the
// answers by the keys it chose. The library asks the client in the way the revision of the call prescribes.

// What a handler of a tool or a prompt returns: its text, or a request for input.
using Outcome = std::variant<std::string, neomcp::InputRequired>;

inline neomcp::InputRequired asking(std::string key, neomcp::InputRequest request,
                                    std::optional<std::string> state = std::nullopt) {
    neomcp::InputRequired ask;
    ask.requests.emplace(std::move(key), std::move(request));
    ask.state = std::move(state);
    return ask;
}

// The answer of kind `Answer` that the last round gave to `key`, or null.
template <class Answer>
const Answer* answer_to(const neomcp::Context& call, const std::string& key) {
    const auto found = call.input_responses().find(key);
    return found == call.input_responses().end() ? nullptr : std::get_if<Answer>(&found->second);
}

// The text of a content block, or of the first block of an array, that has one.
inline std::string text_in(const neomcp::json& content) {
    if (content.is_array()) {
        for (const neomcp::json& item : content) {
            const std::string found = text_in(item);
            if (!found.empty()) return found;
        }
        return std::string();
    }
    if (content.is_object() && content.contains("text") && content.at("text").is_string()) {
        return content.at("text").get<std::string>();
    }
    return std::string();
}

inline neomcp::SamplingRequest sampling_of(std::string prompt, std::uint64_t max_tokens) {
    neomcp::SamplingMessage message;
    message.content.emplace_back(text(std::move(prompt)));
    neomcp::SamplingRequest request;
    request.messages.push_back(std::move(message));
    request.max_tokens = max_tokens;
    return request;
}

// A form of one required string.
inline neomcp::ElicitationForm form_of_string(std::string message, const std::string& field) {
    neomcp::json property = neomcp::json::object();
    property["type"] = "string";
    neomcp::json properties = neomcp::json::object();
    properties[field] = std::move(property);
    neomcp::json required = neomcp::json::array();
    required.push_back(field);
    neomcp::json schema = neomcp::json::object();
    schema["type"] = "object";
    schema["properties"] = std::move(properties);
    schema["required"] = std::move(required);
    return neomcp::ElicitationForm{std::move(message), std::move(schema)};
}

inline neomcp::ElicitationForm confirmation_form(std::string message) {
    neomcp::json schema = neomcp::json::parse(
        R"({"type":"object","properties":{"ok":{"type":"boolean"}},"required":["ok"]})");
    return neomcp::ElicitationForm{std::move(message), std::move(schema)};
}

// The text of an answer to a form: what the user did, and what the user entered.
inline std::string described(const neomcp::ElicitationAnswer& answer) {
    std::string action = "cancel";
    if (answer.action == neomcp::ElicitationAction::accept) action = "accept";
    if (answer.action == neomcp::ElicitationAction::decline) action = "decline";
    return "action=" + action + ", content=" + answer.content.dump();
}

// The keys of the answers to three kinds of request, as the tool that asks for all three chose them.
inline constexpr const char* name_key = "user_name";
inline constexpr const char* greeting_key = "greeting";
inline constexpr const char* roots_key = "client_roots";

inline void register_input_tools(neomcp::Server& server) {
    // One request of each kind, each as the plainest tool that asks it.
    server.tool("test_input_required_result_elicitation", "Ask the user for a name, then greet", [](neomcp::Context& call) -> Outcome {
        const auto* answer = answer_to<neomcp::ElicitationAnswer>(call, name_key);
        if (answer == nullptr) return asking(name_key, form_of_string("What is your name?", "name"));
        std::string name = "stranger";
        if (answer->action == neomcp::ElicitationAction::accept && answer->content.contains("name") &&
            answer->content.at("name").is_string()) {
            name = answer->content.at("name").get<std::string>();
        }
        return "Hello, " + name + "!";
    });

    server.tool("test_input_required_result_sampling", "Ask the client's model a question", [](neomcp::Context& call) -> Outcome {
        const auto* answer = answer_to<neomcp::SamplingAnswer>(call, "capital_question");
        if (answer == nullptr) return asking("capital_question", sampling_of("What is the capital of France?", 100));
        return "The model said: " + text_in(answer->content);
    });

    server.tool("test_input_required_result_list_roots", "Ask the client for its roots", [](neomcp::Context& call) -> Outcome {
        const auto* answer = answer_to<neomcp::RootsAnswer>(call, roots_key);
        if (answer == nullptr) return asking(roots_key, neomcp::RootsRequest{});
        std::string listing = "Roots (" + std::to_string(answer->roots.size()) + "):";
        for (const neomcp::Root& root : answer->roots) listing += " " + root.uri;
        return listing;
    });

    // The state the handler gives comes back with the answers, and only then is the call carried out.
    server.tool("test_input_required_result_request_state", "Ask for a confirmation and keep a state meanwhile", [](neomcp::Context& call) -> Outcome {
        constexpr const char* state = "awaiting-confirmation";
        const auto* answer = answer_to<neomcp::ElicitationAnswer>(call, "confirm");
        if (answer == nullptr || call.request_state() != std::optional<std::string>(state)) {
            return asking("confirm", confirmation_form("Please confirm"), std::string(state));
        }
        return std::string("Confirmed, state-ok");
    });

    // Every request that is still unanswered is asked together.
    server.tool("test_input_required_result_multiple_inputs", "Ask a form, the model and the roots at once", [](neomcp::Context& call) -> Outcome {
        constexpr const char* state = "awaiting-three-answers";
        const auto* name = answer_to<neomcp::ElicitationAnswer>(call, name_key);
        const auto* greeting = answer_to<neomcp::SamplingAnswer>(call, greeting_key);
        const auto* roots = answer_to<neomcp::RootsAnswer>(call, roots_key);
        if (name != nullptr && greeting != nullptr && roots != nullptr &&
            call.request_state() == std::optional<std::string>(state)) {
            return "All three answers came: " + text_in(greeting->content);
        }
        neomcp::InputRequired ask;
        ask.state = std::string(state);
        ask.requests.emplace(name_key, form_of_string("What is your name?", "name"));
        ask.requests.emplace(greeting_key, sampling_of("Generate a greeting", 50));
        ask.requests.emplace(roots_key, neomcp::RootsRequest{});
        return ask;
    });

    // Two rounds, each with a state of its own.
    server.tool("test_input_required_result_multi_round", "Ask in two rounds", [](neomcp::Context& call) -> Outcome {
        constexpr const char* first = "after-step-1";
        constexpr const char* second = "after-step-2";
        const std::optional<std::string>& state = call.request_state();
        if (state == std::optional<std::string>(second) && answer_to<neomcp::ElicitationAnswer>(call, "step2") != nullptr) {
            return std::string("Both steps are done");
        }
        if (state == std::optional<std::string>(first) && answer_to<neomcp::ElicitationAnswer>(call, "step1") != nullptr) {
            return asking("step2", form_of_string("Step 2: What is your favorite color?", "color"), std::string(second));
        }
        return asking("step1", form_of_string("Step 1: What is your name?", "name"), std::string(first));
    });

    // The library signs the state that travels to the client and refuses one that was changed; the handler only asks.
    server.tool("test_input_required_result_tampered_state", "Ask for a confirmation whose state the client must return unchanged", [](neomcp::Context& call) -> Outcome {
        if (answer_to<neomcp::ElicitationAnswer>(call, "confirm") == nullptr) {
            return asking("confirm", confirmation_form("Please confirm"), std::string("signed-state"));
        }
        return std::string("The state was accepted");
    });

    // Asks for what the client declared it can take: a form, else its model.
    server.tool("test_input_required_result_capabilities", "Ask for what the client can answer", [](neomcp::Context& call) -> Outcome {
        if (!call.input_responses().empty()) return std::string("Answered");
        const neomcp::InputRequest form = form_of_string("What is your name?", "name");
        if (call.client_supports(form)) return asking("user_name", form);
        const neomcp::InputRequest model = sampling_of("Generate a greeting", 50);
        if (call.client_supports(model)) return asking("greeting", model);
        return std::string("The client can answer neither a form nor a sampling request");
    });

    // The tools that the runs of the revisions with a handshake call, and the ones the stateless run uses to find
    // out what happens when the client lacks a capability or the server asks while it streams.
    server.tool<PromptArgument>("test_sampling", "Ask the client's model for a completion of a prompt", [](const PromptArgument& argument, neomcp::Context& call) -> Outcome {
        const auto* answer = answer_to<neomcp::SamplingAnswer>(call, "sample");
        if (answer == nullptr) return asking("sample", sampling_of(argument.prompt, 100));
        return "LLM response: " + text_in(answer->content);
    });

    server.tool<MessageArgument>("test_elicitation", "Ask the user for a user name and an email address", [](const MessageArgument& argument, neomcp::Context& call) -> Outcome {
        const auto* answer = answer_to<neomcp::ElicitationAnswer>(call, "form");
        if (answer == nullptr) {
            neomcp::json schema = neomcp::json::parse(
                R"({"type":"object","properties":{"username":{"type":"string","description":"User's response"},)"
                R"("email":{"type":"string","description":"User's email address"}},"required":["username","email"]})");
            return asking("form", neomcp::ElicitationForm{argument.message, std::move(schema)});
        }
        return "User response: " + described(*answer);
    });

    server.tool("test_elicitation_sep1034_defaults", "Ask for a form whose fields all have a default", [](neomcp::Context& call) -> Outcome {
        const auto* answer = answer_to<neomcp::ElicitationAnswer>(call, "form");
        if (answer == nullptr) {
            neomcp::json schema = neomcp::json::parse(R"({
                "type":"object",
                "properties":{
                    "name":{"type":"string","default":"John Doe"},
                    "age":{"type":"integer","default":30},
                    "score":{"type":"number","default":95.5},
                    "status":{"type":"string","enum":["active","inactive","pending"],"default":"active"},
                    "verified":{"type":"boolean","default":true}}})");
            return asking("form", neomcp::ElicitationForm{"Please provide your information", std::move(schema)});
        }
        return "Elicitation completed: " + described(*answer);
    });

    server.tool("test_elicitation_sep1330_enums", "Ask for a form with every kind of choice", [](neomcp::Context& call) -> Outcome {
        const auto* answer = answer_to<neomcp::ElicitationAnswer>(call, "form");
        if (answer == nullptr) {
            neomcp::json schema = neomcp::json::parse(R"({
                "type":"object",
                "properties":{
                    "untitledSingle":{"type":"string","enum":["option1","option2","option3"]},
                    "titledSingle":{"type":"string","oneOf":[
                        {"const":"value1","title":"First Option"},
                        {"const":"value2","title":"Second Option"},
                        {"const":"value3","title":"Third Option"}]},
                    "legacyEnum":{"type":"string","enum":["opt1","opt2","opt3"],
                        "enumNames":["Option One","Option Two","Option Three"]},
                    "untitledMulti":{"type":"array","items":{"type":"string","enum":["option1","option2","option3"]}},
                    "titledMulti":{"type":"array","items":{"anyOf":[
                        {"const":"value1","title":"First Choice"},
                        {"const":"value2","title":"Second Choice"},
                        {"const":"value3","title":"Third Choice"}]}}}})");
            return asking("form", neomcp::ElicitationForm{"Select your preferences", std::move(schema)});
        }
        return "Elicitation completed: " + described(*answer);
    });

    // Needs the client's model: a call from a client that did not declare it is refused for that.
    server.tool("test_missing_capability", "Ask the client's model, which the client must be able to take", [](neomcp::Context& call) -> Outcome {
        const auto* answer = answer_to<neomcp::SamplingAnswer>(call, "sample");
        if (answer == nullptr) return asking("sample", sampling_of("Say hello", 20));
        return "The model said: " + text_in(answer->content);
    });

    // Asks the user while the call is served; where the result asks, nothing but the result is sent to the client.
    server.tool("test_streaming_elicitation", "Ask the user a question while the call is served", [](neomcp::Context& call) -> Outcome {
        const auto* answer = answer_to<neomcp::ElicitationAnswer>(call, "question");
        if (answer == nullptr) return asking("question", form_of_string("What is your name?", "name"));
        return "Elicitation completed: " + described(*answer);
    });
}

inline void register_input_prompt(neomcp::Server& server) {
    server.prompt("test_input_required_result_prompt", "A prompt that asks the user for its context", [](neomcp::Context& call) -> Outcome {
        const auto* answer = answer_to<neomcp::ElicitationAnswer>(call, "user_context");
        if (answer == nullptr) return asking("user_context", form_of_string("What context should the prompt use?", "context"));
        std::string context = "no context";
        if (answer->action == neomcp::ElicitationAction::accept && answer->content.contains("context") &&
            answer->content.at("context").is_string()) {
            context = answer->content.at("context").get<std::string>();
        }
        return "A prompt about: " + context;
    });
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
    register_input_tools(server);
    register_resources(server);
    register_prompts(server);
    register_input_prompt(server);
}

}  // namespace conformance
