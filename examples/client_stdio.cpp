// Example 3: a client that starts example 1 as a child process and uses what the server offers.
//
// neomcp::Client::open starts the program, speaks MCP over its standard input and output, and returns once the two
// have agreed on a revision of the protocol. From then on the client's methods are the protocol's operations: list the
// tools, call one, read a resource, get a prompt. Each returns a neomcp::Result, which holds the value or a
// ClientError.
//
// Usage: client_stdio [path of the stdio_server program]
// Without an argument the program is looked for next to this one.
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <variant>

#include <neomcp/client.h>
#include <neomcp/client/stdio.h>

#include "client_support.h"
#include "demo_types.h"

namespace {
constexpr const char* program = "client_stdio";
}

int main(int argc, char** argv) {
    // Where the server program is: given, or a sibling of this program.
    std::filesystem::path server_path;
    if (argc > 1) {
        server_path = argv[1];
    } else {
        server_path = std::filesystem::path(argv[0]);
        server_path.replace_filename("stdio_server" + server_path.extension().string());
    }

    // What the client says about itself, and the process to start. The child gets only a short list of variables of
    // this program's environment unless `inherited` or `environment` name more, so what this program's environment
    // holds does not reach a server by accident.
    neomcp::ClientOptions options;
    options.client_info.name = "example-client";
    options.client_info.version = "1.0.0";
    neomcp::ChildProcess process;
    process.program = server_path.string();

    auto opened = neomcp::Client::open(options, std::move(process));
    if (!opened) return report(program, "opening the client", opened.error());
    neomcp::Client client = std::move(opened).value();

    // What the server said about itself, and the revision that was agreed.
    const neomcp::ServerDescription server = client.server();
    std::cout << "server: " << (server.info ? server.info->name : "(unnamed)") << ", revision " << server.revision
              << '\n';

    // The tools, every page of the list.
    const auto tools = client.list_all_tools();
    if (!tools) return report(program, "listing the tools", tools.error());
    for (const neomcp::ToolDefinition& tool : tools->items) {
        std::cout << "tool: " << tool.name << " - " << tool.description.value_or("") << '\n';
    }
    if (tools->items.size() != 3) return unexpected(program, "the server should offer three tools");

    // A typed call: the arguments are a C++ struct, and the result is decoded into one. The client checks the result
    // against the tool's output schema before it decodes it.
    const auto sum = client.call<std::int64_t>(add_tool_name, AddArgs{2, 3});
    if (!sum) return report(program, "calling add", sum.error());
    if (sum->is_error || !sum->value || *sum->value != 5) return unexpected(program, "2 + 3 should be 5");
    std::cout << "2 + 3 = " << *sum->value << '\n';

    const auto division = client.call<Division>(divide_tool_name, DivideArgs{17, 5});
    if (!division) return report(program, "calling divide", division.error());
    if (division->is_error || !division->value || division->value->quotient != 3 || division->value->remainder != 2) {
        return unexpected(program, "17 / 5 should be 3 remainder 2");
    }
    std::cout << "17 / 5 = " << division->value->quotient << " remainder " << division->value->remainder << '\n';

    // A tool that fails answers with a result that says so. The call itself succeeded, so this is a value, not a
    // ClientError: `is_error` is set and the typed `value` is empty.
    const auto by_zero = client.call<Division>(divide_tool_name, DivideArgs{1, 0});
    if (!by_zero) return report(program, "calling divide by zero", by_zero.error());
    if (!by_zero->is_error || by_zero->value) return unexpected(program, "dividing by zero should be a tool error");
    std::cout << "1 / 0: the tool said \"" << first_text(*by_zero) << "\"\n";

    // The untyped form takes and gives JSON, for a tool whose types the client does not have.
    const auto stored = client.call_tool(add_note_tool_name, neomcp::json{{"text", "buy milk"}});
    if (!stored) return report(program, "calling add_note", stored.error());
    std::cout << "add_note: " << first_text(*stored) << '\n';

    // Reading a resource by its URI.
    const auto notes = client.read_resource(notes_uri);
    if (!notes) return report(program, "reading the notes", notes.error());
    const auto* text =
        notes->contents.empty() ? nullptr : std::get_if<neomcp::TextResourceContents>(&notes->contents[0]);
    if (text == nullptr || text->text != "buy milk\n") return unexpected(program, "the notes should hold one note");
    std::cout << "notes:\n" << text->text;

    // A prompt, with its arguments by name. The result is the messages to give the model.
    const auto prompt = client.get_prompt(summary_prompt_name, {{"style", "one line"}});
    if (!prompt) return report(program, "getting the prompt", prompt.error());
    if (prompt->messages.size() != 1) return unexpected(program, "the prompt should be one message");
    if (const auto* message = std::get_if<neomcp::TextContent>(&prompt->messages[0].content)) {
        std::cout << "prompt:\n" << message->text;
    }

    std::cout << "client_stdio: ok\n";
    return 0;
}
