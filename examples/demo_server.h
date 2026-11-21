// The server that examples 1, 2 and 8 serve (the types of its tools and its names are in demo_types.h, which the
// clients of examples 3 and 4 include).
//
// An MCP server offers three kinds of things to a client:
//
//   tools      things the client's model can ask the server to do (add two numbers, store a note);
//   resources  things the client can read by address (the list of notes);
//   prompts    message templates the client's user can pick (summarise the notes in some style).
//
// This header registers one of each kind on a neomcp::Server. The subjects are a small calculator and a list of notes
// that lives in memory for as long as the program runs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <neomcp/server.h>

#include "demo_types.h"

// The notes. Handlers run on the server's worker threads, several at once, so the list has a lock.
class Notes {
public:
    // Stores a note and returns how many notes there are now.
    std::size_t add(std::string text) {
        const std::lock_guard<std::mutex> lock(mutex_);
        items_.push_back(std::move(text));
        return items_.size();
    }

    // Forgets the first note with this text; false when there is none.
    bool forget(const std::string& text) {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (auto note = items_.begin(); note != items_.end(); ++note) {
            if (*note == text) {
                items_.erase(note);
                return true;
            }
        }
        return false;
    }

    std::size_t size() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

    // One note per line.
    std::string text() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::string all;
        for (const std::string& item : items_) {
            all += item;
            all += '\n';
        }
        return all;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> items_;
};

// The two registrations that example 8 sets scopes on, which only works before the server serves.
struct DemoHandles {
    neomcp::ToolHandle add_note;
    neomcp::ResourceHandle notes;
};

// Registers the tools, the resource and the prompt. Registration ends when the first client connects: after that,
// registering anything throws. `server` and `notes` must outlive every call, so declare `notes` before `server`.
inline DemoHandles add_demo_features(neomcp::Server& server, Notes& notes) {
    // A handler is any callable that takes the arguments type. What it returns becomes the tool's result: a number
    // is wrapped as {"result": 5}, a described struct is returned as it is, a std::string is plain text.
    server.tool<AddArgs>(add_tool_name, "Add two integers", [](const AddArgs& args) {
        return static_cast<std::int64_t>(args.a) + args.b;
    });

    // Throwing neomcp::ToolError answers the call with a tool error, a result that says the tool failed and why. The
    // call itself succeeds: the model sees the message and can react to it.
    server.tool<DivideArgs>(divide_tool_name, "Divide one integer by another", [](const DivideArgs& args) {
        if (args.divisor == 0) throw neomcp::ToolError("cannot divide by zero");
        const std::int64_t dividend = args.dividend;
        const std::int64_t divisor = args.divisor;
        return Division{dividend / divisor, dividend % divisor};
    });

    // A tool that changes the server's state. It tells the clients that watch the notes resource that its content
    // changed.
    const neomcp::ToolHandle add_note =
        server.tool<NoteArgs>(add_note_tool_name, "Store a note", [&server, &notes](const NoteArgs& args) {
            const std::size_t count = notes.add(args.text);
            server.notify_resource_updated(notes_uri);
            return "stored note " + std::to_string(count);
        });

    // A resource has a fixed address (its URI) and a name. A client lists the resources and reads one by its URI.
    const neomcp::ResourceHandle notes_resource =
        server.resource(notes_uri, "All notes", [&notes] { return notes.text(); }).mime_type("text/plain");

    // A prompt is a template of messages. A handler that returns a std::string gives one message of the user.
    server.prompt<SummaryArgs>(summary_prompt_name, "Ask for a summary of the notes",
                               [&notes](const SummaryArgs& args) {
                                   return "Summarise these notes (" + args.style + "):\n\n" + notes.text();
                               });

    return DemoHandles{add_note, notes_resource};
}
