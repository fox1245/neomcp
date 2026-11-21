// The types and names of the demonstration server (demo_server.h is the server itself). A client that has them calls
// the server's tools with the same C++ types the server declares them from.
#pragma once

#include <cstdint>
#include <string>

#include <neomcp/describe.h>

// ---- Tools declared from C++ types
//
// A tool's arguments are a C++ struct. NEOMCP_DESCRIBE lists its members, each with a description, and from that the
// library derives the tool's JSON Schema. Before a handler runs, the arguments a client sent are checked against that
// schema, so a handler only ever sees arguments that have the right shape.

struct AddArgs {
    int a;
    int b;
};
NEOMCP_DESCRIBE(AddArgs, a, "first addend", b, "second addend")

struct DivideArgs {
    int dividend;
    int divisor;
};
NEOMCP_DESCRIBE(DivideArgs, dividend, "the number to divide", divisor, "the number to divide by (not zero)")

// A tool may return a described struct too. The client gets it as structured content, and the tool's output schema,
// derived from the same type, lets a client check that what arrived has the promised shape.
struct Division {
    std::int64_t quotient;
    std::int64_t remainder;
};
NEOMCP_DESCRIBE(Division, quotient, "how many times the divisor fits", remainder, "what is left over")

struct NoteArgs {
    std::string text;
};
NEOMCP_DESCRIBE(NoteArgs, text, "the text of the note")

// The arguments of a prompt are strings on the wire: a described struct of std::string members (required) and
// std::optional<std::string> members (optional).
struct SummaryArgs {
    std::string style;
};
NEOMCP_DESCRIBE(SummaryArgs, style, "how to summarise, for example \"one line\" or \"bullet points\"")

// The address of the resource that holds every note, and the names of the tools and the prompt, so that the clients of
// the other examples use the same ones.
inline constexpr const char* notes_uri = "notes://all";
inline constexpr const char* add_tool_name = "add";
inline constexpr const char* divide_tool_name = "divide";
inline constexpr const char* add_note_tool_name = "add_note";
inline constexpr const char* summary_prompt_name = "summarize_notes";
