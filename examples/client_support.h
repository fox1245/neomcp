// Small helpers shared by the client examples: reading a tool's text and reporting an error. Nothing here is part of
// the library.
#pragma once

#include <iostream>
#include <string>
#include <string_view>
#include <variant>

#include <neomcp/client.h>

// The text of the first text block of a tool's result; empty when there is none.
inline std::string first_text(const neomcp::ToolResult& result) {
    for (const neomcp::ContentBlock& block : result.content) {
        if (const auto* text = std::get_if<neomcp::TextContent>(&block)) return text->text;
    }
    return {};
}

// Every operation of the client returns a neomcp::Result: the value, or a ClientError that says in `kind` what went
// wrong (the server answered with an error, the call timed out, the server exited, ...) and in `message` how, in
// words. Nothing is thrown for such failures.
inline int report(std::string_view program, std::string_view step, const neomcp::ClientError& error) {
    std::cerr << program << ": " << step << " failed: " << neomcp::to_string(error.kind) << ": " << error.message
              << '\n';
    return 1;
}

// A check of the example itself: what the program found is not what it expects.
inline int unexpected(std::string_view program, std::string_view what) {
    std::cerr << program << ": unexpected result: " << what << '\n';
    return 1;
}
