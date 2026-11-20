// The child-process transport (neomcp::client_stdio): the client starts the server as a child process and speaks to it
// over the child's standard input and output, one message per line.
//
// Client::open(options, ChildProcess{...}) makes this transport with the client's own bound on a message
// (ClientOptions::max_message_bytes). make_transport(ChildProcess, ChildProcessTransportOptions) makes it for
// Client::open(options, std::unique_ptr<Transport>) when the application wants to watch the process as well.
//
// Starting. The program is found before the start: a name without a directory through the PATH of the environment the
// child gets, any other path relative to the working directory of the child when one is given. No shell is involved,
// and the arguments reach the program as they are. The child gets the variables named by ChildProcess::inherited
// from the client's environment (when it is empty, the names of inherited_environment(); an empty list names none),
// then ChildProcess::environment; nothing else.
// The program runs with the rights of the application that starts it.
// - POSIX: the child is the leader of a process group of its own; it holds no descriptor of the client's but its
//   standard streams, and starts with every signal at its default disposition and none blocked. One thread per child
//   waits for its end.
// - Windows: the child is started suspended and put in a job object before it runs, so that every process it starts
//   belongs to the job; it inherits its three standard handles and no other. A program named without an extension is
//   looked for with ".exe"; a batch file (.bat, .cmd) is refused, since Windows would run it through the command
//   interpreter.
// A process is started again when the client asks for it (the server ended while the client went on); the program is
// looked for again each time.
//
// Reading. Every line of the child's standard output is one message; a '\r' before the line feed is dropped. A line
// that is not JSON, or longer than the bound on a message, is skipped, and the client is told; a process that writes
// more such lines (or bytes) than ChildProcess allows is ended, as one that broke the protocol. The counts start
// again with each process.
//
// The end of a process. When the leader of the group ends, by itself or because it was asked to, the rest of its
// group (POSIX) or its job (Windows) is ended too, the output it wrote before it ended is read for at most
// ChildProcess::exit_grace, and then the client is told that the server ended (with the exit status when it exited).
// Closing the transport in order (the client closes) closes the child's input, waits ChildProcess::exit_grace for it
// to exit, then (POSIX) asks its group to terminate and waits ChildProcess::term_grace, and then ends the group (POSIX)
// or the job (Windows). Closing because contact cannot go on closes the input and ends the group at once. A process
// whose input cannot be written to, or that closed its output and did not exit within exit_grace, is ended in the
// same way at once.
//
// When another part of the program collects the child's exit first (POSIX: a handler of SIGCHLD that waits for any
// child, or SIGCHLD ignored), the end is still noticed, but without a status, and the group is not signalled any
// more, since its number may already name another group.
//
// Threads. As for every transport: its methods are called on the client's thread, and the observer and
// ChildProcess::on_stderr_line run there; each must return promptly.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "neomcp/client/engine.h"
#include "neomcp/client/result.h"
#include "neomcp/client/transport.h"

namespace neomcp {

// One step in the life of a child process.
enum class ChildProcessStep {
    started,         // a process runs (ChildProcessEvent::pid)
    input_closed,    // its standard input was closed
    terminate_sent,  // POSIX: its process group was asked to terminate (SIGTERM)
    kill_sent,       // its process group was ended (POSIX: SIGKILL) or its job terminated (Windows)
    exited           // the process ended and was collected; exit_code or signal says how, when it is known
};

// Why the step was taken.
enum class ChildProcessCause {
    start,              // started: the transport started
    restart,            // started: the client asked for a new process after the server ended
    closing,            // the client closed the transport in order
    contact_ended,      // the client closed the transport because contact cannot go on
    exit_grace_lapsed,  // the process did not exit within exit_grace after its input was closed
    term_grace_lapsed,  // POSIX: the process did not exit within term_grace after its group was asked to terminate
    process_ended,      // the leader ended: what is left of its group or job is ended, and its input closed
    output_refused,     // the process wrote more lines that are not messages than ChildProcess allows
    output_closed,      // the process closed its standard output and did not exit within exit_grace
    input_failed        // a message could not be written to the process's standard input
};

struct ChildProcessEvent {
    ChildProcessStep step = ChildProcessStep::started;
    ChildProcessCause cause = ChildProcessCause::start;
    // The process id of the process the step concerns.
    std::int64_t pid = 0;
    // exited: the exit status when the process exited, the signal that ended it (POSIX) when it was ended by one;
    // both empty when the end was collected elsewhere.
    std::optional<int> exit_code;
    std::optional<int> signal;
    // exited, StderrMode::capture: the last bytes the process wrote to its standard error (at most
    // ChildProcess::stderr_tail_bytes), as it wrote them. Text of the server: show it, never act on it.
    std::string stderr_tail;
};

// Told of each step, on the client's thread; it must return promptly. What it throws is dropped.
using ChildProcessObserver = std::function<void(const ChildProcessEvent& event)>;

// What a child-process transport takes besides the ChildProcess.
struct ChildProcessTransportOptions {
    ChildProcessObserver observer;
    // A line of the server's standard output longer than this is skipped unread. Client::open uses
    // ClientOptions::max_message_bytes. Zero is an error. Default 16 MiB.
    std::size_t max_message_bytes = ClientDefaults::max_message_bytes;
};

// A child-process transport with an observer. Fails with invalid_options for a description that cannot work (an empty
// program; a NUL byte in the program, an argument, a name or a value; a variable name that is empty or holds '=';
// max_stderr_line_bytes or max_message_bytes zero; a negative grace). A program that cannot be found or started fails
// Transport::start, and so Client::open with ClientErrorKind::transport.
Result<std::unique_ptr<Transport>> make_transport(ChildProcess process, ChildProcessTransportOptions options);

}  // namespace neomcp
