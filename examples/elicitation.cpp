// Example 6: a tool that asks the client for input, and a client that answers. It is the complete program of the
// snippet in the README's section "Asking the client".
//
// Sometimes a tool needs something only the person at the client can give: a confirmation before it does something
// that cannot be undone, a choice, a value. MCP calls that elicitation. The tool declares what it asks (here a form
// with one yes or no question, whose text names the note the call is about), and the client answers through a handler
// it registers. Both run in this one process, joined through memory as in example 5.
//
// The server's code is the same whichever revision the client speaks, though the protocol carries the question
// differently: in the older era the server sends a request to the client while the call waits, and in the newer one
// the call's result asks and the client calls again with the answer. This program runs the call under the newest
// revision of each era to show that.
#include <atomic>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <neomcp/client.h>
#include <neomcp/client/in_process.h>
#include <neomcp/profile.h>
#include <neomcp/server.h>

#include "client_support.h"
#include "demo_server.h"

// The arguments of the tool: which note to forget.
struct ForgetArgs {
    std::string text;
};
NEOMCP_DESCRIBE(ForgetArgs, text, "the text of the note to forget")

// The form the user fills in: one yes or no question. The form is built from this type, and what the user submits is
// checked against it before the handler sees it. (NEOMCP_DESCRIBE is written outside any namespace.)
struct Confirm {
    bool ok;
};
NEOMCP_DESCRIBE(Confirm, ok, "Forget it?")

namespace {

constexpr const char* program = "elicitation";

// The newest built-in revision of each era.
std::vector<std::string> newest_of_each_era() {
    std::string legacy;
    std::string modern;
    for (const std::string_view revision : neomcp::profile::builtin_revisions()) {  // oldest first
        const bool is_modern = neomcp::profile::builtin(revision)->era() == neomcp::profile::Era::modern;
        (is_modern ? modern : legacy) = std::string(revision);
    }
    return {legacy, modern};
}

// Calls the tool once under `revision`, with the person answering `say_yes`.
int run(const std::string& revision, bool say_yes) {
    Notes notes;
    notes.add("first");
    notes.add("second");

    neomcp::Server server({.name = "elicitation", .version = "1.0.0"});
    // The input is declared after the handler: the key "confirm" names it, the type Confirm is the form, and the
    // function gives the message shown with it, built from the arguments. It runs on every round of the call, so it
    // has no effects and asks the same question for the same arguments. The handler runs once, when the answer is
    // there, and gets it as neomcp::Answer<Confirm>: it says whether the user accepted, declined or dismissed the form,
    // and holds the submitted Confirm when it accepted.
    server.tool<ForgetArgs>(
        "forget_note", "Forget one note",
        [&notes](const ForgetArgs& args, const neomcp::Answer<Confirm>& confirm) {
            if (!confirm.accepted() || !confirm->ok) return std::string("kept the note");
            return std::string(notes.forget(args.text) ? "forgot the note" : "there was no such note");
        },
        neomcp::elicit<Confirm>("confirm", [](const ForgetArgs& args) {
            return "Forget the note \"" + args.text + "\"?";
        }));

    // The client speaks only `revision`.
    neomcp::ClientOptions options;
    options.client_info.name = "example-client";
    options.client_info.version = "1.0.0";
    options.profiles.push_back(neomcp::profile::builtin(revision));

    // The client's answer to a form. It gets the form (the message and the schema) and returns what the person did:
    // accept with the filled-in values, decline, or cancel. A handler may run on several threads at once, so it only
    // reads what it captures.
    const std::atomic<bool> answer_yes{say_yes};
    options.handlers.form([&answer_yes](const neomcp::ElicitationForm& form) {
        std::cout << "  the server asks: " << form.message << " -> " << (answer_yes ? "yes" : "no") << '\n';
        neomcp::ElicitationAnswer answer;
        if (answer_yes) {
            answer.action = neomcp::ElicitationAction::accept;
            answer.content = neomcp::json{{"ok", true}};
        } else {
            answer.action = neomcp::ElicitationAction::decline;
        }
        return answer;
    });

    auto peer = neomcp::InProcessServer::create(server);
    auto opened = neomcp::Client::open(options, peer->transport());
    if (!opened) return report(program, "opening the client", opened.error());

    const auto called = opened->call_tool("forget_note", neomcp::json{{"text", "first"}});
    if (!called) return report(program, "calling forget_note", called.error());
    const std::string text = first_text(*called);
    std::cout << "  the tool answered: " << text << " (notes left: " << notes.size() << ")\n";

    const std::string expected = say_yes ? "forgot the note" : "kept the note";
    const std::size_t notes_left = say_yes ? 1 : 2;
    if (text != expected || notes.size() != notes_left) {
        return unexpected(program, "the notes were not as the answer says");
    }
    return 0;
}

}  // namespace

int main() {
    for (const std::string& revision : newest_of_each_era()) {
        for (const bool say_yes : {false, true}) {
            std::cout << "revision " << revision << ", the person says " << (say_yes ? "yes" : "no") << '\n';
            if (const int status = run(revision, say_yes)) return status;
        }
    }
    std::cout << "elicitation: ok\n";
    return 0;
}
