// The stdio transport: a server speaks to one client over the process's standard input and output.
//
//   int main() {
//       neomcp::Server server({.name = "calc", .version = "1.0.0"});
//       server.tool<AddArgs>("add", "Add two integers", [](const AddArgs& args) { return args.a + args.b; });
//       return neomcp::run_stdio(server);
//   }
//
// Framing, in both eras: one JSON-RPC message per line, each line ends with '\n', a '\r' before it is dropped, empty
// lines are skipped, and a last line without a terminator at the end of input is a line too. Output is one line per
// message, each followed by '\n', and nothing else is ever written to the output stream by this library; the
// library's own diagnostics go to standard error (StdioOptions::diagnostics).
//
// Input is bounded: a line longer than ServerOptions::max_message_bytes is never held whole. At most that many bytes
// plus one are kept and the rest of the line is skipped, and the session answers the kept text with Invalid Request.
//
// End of input. The specification asks a server to exit promptly when its input closes. So the transport stops
// reading, waits until the calls in flight have been answered and written, for at most StdioOptions::drain_timeout,
// then closes the session, which requests stop on whatever still runs, and returns. A client may therefore write its
// requests, close its end and still read every answer (a pipe from a shell does that).
//
// A failed write (the reader of the output is gone) closes the session and ends the transport. The transport learns
// of it on the thread that wrote, and ends the blocked read through ByteReader::interrupt() where the reader can.
#pragma once

#include <chrono>
#include <cstddef>
#include <string_view>

#include "neomcp/byte_stream.h"
#include "neomcp/server.h"

namespace neomcp {

struct StdioOptions {
    // How long, after the input ended, the transport waits for the calls in flight to be answered and written before
    // it closes the session. Zero: close at once. Default 5 s. The specification asks for a prompt exit when input
    // closes, and a client that wants to stop a server closes its end and waits a few seconds before it terminates the
    // process (the Python SDK's client waits 2 s), so a long drain buys nothing there; a client that closes its end
    // and keeps reading (a pipe from a shell) gets every answer of a call that finishes within this time.
    std::chrono::milliseconds drain_timeout{5000};

    // The size of the buffer one read() fills. Zero is an error (std::invalid_argument). Default 64 KiB.
    std::size_t read_chunk_bytes = std::size_t{64} * 1024;

    // Write the library's own diagnostics to standard error: why the session could not be created, and a failed
    // write. Nothing else is ever written there by the library.
    bool diagnostics = true;

    // POSIX: ignore SIGPIPE while the transport runs and restore the previous disposition when it returns, so that a
    // write to a closed pipe fails with an error instead of ending the process. The disposition is process-wide: it
    // holds for every thread of the process while the transport runs. No effect on Windows.
    bool ignore_sigpipe = true;
};

// Serves one client over the given streams until the input ends, then drains and returns (see above). Returns 0
// after a normal end of input and after a failed write, and 1 when the session could not be created (the server's
// profiles name a mechanism that has no implementation; the reason goes to standard error when diagnostics are on).
// The server must outlive the call. Throws std::invalid_argument for options that cannot work.
int serve_streams(Server& server, ByteReader& input, ByteWriter& output, StdioOptions options = {});

// serve_streams over the process's standard input and output (file descriptors 0 and 1). On Windows both are put in
// binary mode while it runs and restored when it returns. Returns what serve_streams returns.
int run_stdio(Server& server, StdioOptions options = {});

}  // namespace neomcp
