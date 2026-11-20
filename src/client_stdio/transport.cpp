// The child-process transport (neomcp/client/stdio.h): lines in both directions, the bounds on what is not a message,
// the steps that end a process, and starting a new one when the client asks.
#include <array>
#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <asio/awaitable.hpp>
#include <asio/buffer.hpp>
#include <asio/post.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "client_stdio/child.h"
#include "io/fd_streams.h"
#include "io/line_splitter.h"
#include "neomcp/client.h"
#include "neomcp/client/stdio.h"

namespace neomcp {

namespace detail {

namespace {

// How much one read takes from a pipe.
constexpr std::size_t read_chunk_bytes = std::size_t{64} * 1024;

ClientError invalid(std::string message) {
    ClientError error;
    error.kind = ClientErrorKind::invalid_options;
    error.message = std::move(message);
    return error;
}

bool holds_nul(std::string_view text) { return text.find('\0') != std::string_view::npos; }

// Why a description cannot work; empty when it can.
std::optional<std::string> check(const ChildProcess& process, const ChildProcessTransportOptions& options) {
    std::string problems;
    if (process.program.empty()) problems += "the program is empty\n";
    if (holds_nul(process.program)) problems += "the program holds a NUL byte\n";
    for (const std::string& argument : process.arguments) {
        if (holds_nul(argument)) {
            problems += "an argument holds a NUL byte\n";
            break;
        }
    }
    const auto check_name = [&problems](std::string_view name) {
        if (name.empty()) {
            problems += "the name of a variable is empty\n";
        } else if (name.find('=') != std::string_view::npos || holds_nul(name)) {
            problems += "the name of a variable holds '=' or a NUL byte\n";
        }
    };
    if (process.inherited) {
        for (const std::string& name : *process.inherited) check_name(name);
    }
    for (const auto& [name, value] : process.environment) {
        check_name(name);
        if (holds_nul(value)) problems += "the value of a variable holds a NUL byte\n";
    }
    if (process.working_directory && process.working_directory->empty()) problems += "the working directory is empty\n";
    if (process.stderr_mode == StderrMode::capture && process.max_stderr_line_bytes == 0) {
        problems += "max_stderr_line_bytes must be positive\n";
    }
    if (process.exit_grace.count() < 0 || process.term_grace.count() < 0) problems += "a grace must not be negative\n";
    if (options.max_message_bytes == 0) problems += "max_message_bytes must be positive\n";
    if (problems.empty()) return std::nullopt;
    problems.pop_back();
    return problems;
}

// One process the transport started, and what is known of it. Shared with the operations that run for it, so that an
// operation that completes after the process was replaced or the transport closed finds it and does nothing.
struct Run {
    Run(const asio::any_io_executor& executor, std::size_t max_line_bytes, std::size_t max_stderr_line_bytes)
        : lines(max_line_bytes), error_lines(max_stderr_line_bytes), wake(executor), bound(executor) {
        wake.expires_at(asio::steady_timer::time_point::max());
    }

    std::unique_ptr<Child> child;
    std::int64_t pid = 0;

    bool input_open = true;
    bool output_ended = false;  // its standard output reached its end (or could not be read)
    bool error_ended = true;    // the same for a captured standard error; true when it is not captured
    bool exited = false;        // the leader ended
    bool observed = false;      // its end was seen here, with a status
    bool terminate_sent = false;
    bool kill_sent = false;
    bool collected = false;
    bool bound_lapsed = false;  // the bound on reading the output of an ended process lapsed
    bool refused = false;       // it was ended for what it wrote; nothing more of its output is read
    bool finished = false;      // it is collected and its end was reported; nothing of it runs any more
    ChildExit how;

    std::deque<std::string> writes;  // whole lines with their terminator; the first is being written
    bool writing = false;

    LineSplitter lines;
    LineSplitter error_lines;
    std::size_t ignored_lines = 0;
    std::size_t ignored_bytes = 0;
    std::string tail;  // the last bytes of its standard error

    std::array<char, read_chunk_bytes> output_buffer{};
    std::array<char, read_chunk_bytes> error_buffer{};

    // The wait of the steps that end the process, raced against its end: cancelled when it exits or is finished.
    asio::steady_timer wake;
    // After the output ended, how long the process may take to exit; after it exited, how long its output is still read.
    asio::steady_timer bound;
    bool bound_running = false;
};

class Core : public std::enable_shared_from_this<Core> {
public:
    Core(ChildProcess process, ChildProcessTransportOptions options)
        : process_(std::move(process)), options_(std::move(options)) {
        if (process_.inherited) {
            inherited_ = *process_.inherited;
        } else {
            for (const std::string_view name : inherited_environment()) inherited_.emplace_back(name);
        }
    }

    std::optional<std::string> start(TransportEvents& events) {
        events_ = &events;
        executor_ = events.executor();
        sigpipe_.emplace(true);
        return spawn(ChildProcessCause::start);
    }

    void send(Outgoing outgoing) {
        if (closing_ || closed_) return;
        switch (outgoing.kind) {
            case OutgoingKind::restart:
                asio::post(executor_, [self = shared_from_this()] { self->restart(); });
                return;
            case OutgoingKind::request:
            case OutgoingKind::notification:
            case OutgoingKind::response: break;
            case OutgoingKind::end_unit:
            case OutgoingKind::open_connection_stream:
            case OutgoingKind::close: return;
        }
        const std::shared_ptr<Run> run = run_;
        if (!run || run->finished) {
            // No process runs, and the client was told so: the message cannot go anywhere.
            asio::post(executor_, [self = shared_from_this()] {
                if (self->deliverable()) {
                    self->events_->stream_event(connection_unit, SendFailed{"the server process is not running"});
                }
            });
            return;
        }
        // A process that is ending: its end is reported, and with it every request that was waiting for it.
        if (run->exited || !run->input_open) return;
        outgoing.line.push_back('\n');
        run->writes.push_back(std::move(outgoing.line));
        if (!run->writing) write_next(run);
    }

    asio::awaitable<void> close(CloseCause cause) {
        if (closing_ || closed_) co_return;
        closing_ = true;
        const std::shared_ptr<Run> run = run_;
        if (run && !run->finished) {
            const ChildProcessCause why =
                cause == CloseCause::client_closed ? ChildProcessCause::closing : ChildProcessCause::contact_ended;
            // A process that ended already is finished without waiting for the rest of its output.
            finish_if_ready(run);
            close_input(run, why);
            if (cause == CloseCause::client_closed) {
                co_await wait(run, process_.exit_grace);
                if constexpr (Child::can_terminate) {
                    if (!run->exited) {
                        terminate(run, ChildProcessCause::exit_grace_lapsed);
                        co_await wait(run, process_.term_grace);
                        if (!run->exited) kill(run, ChildProcessCause::term_grace_lapsed);
                    }
                } else {
                    if (!run->exited) kill(run, ChildProcessCause::exit_grace_lapsed);
                }
            } else {
                kill(run, why);
            }
            while (!run->finished) co_await wait_until_finished(run);
        }
        closed_ = true;
        events_ = nullptr;
        sigpipe_.reset();
    }

    std::size_t max_message_bytes() const noexcept { return options_.max_message_bytes; }

    // Close was never called (the transport was destroyed without it): the process is ended, and collected when the
    // last operation on it lets it go.
    void abandon() noexcept {
        closed_ = true;
        if (run_ && run_->child && !run_->collected) run_->child->kill();
    }

private:
    // ---- Starting

    std::optional<std::string> spawn(ChildProcessCause cause) {
        ChildLaunch launch;
        launch.program = process_.program;
        launch.arguments = process_.arguments;
        launch.working_directory = process_.working_directory;
        launch.environment = child_environment(inherited_, process_.environment);
        launch.stderr_mode = process_.stderr_mode;
        auto run = std::make_shared<Run>(executor_, options_.max_message_bytes, error_line_bound());
        std::weak_ptr<Core> weak_core = weak_from_this();
        std::weak_ptr<Run> weak_run = run;
        auto started = Child::start(executor_, launch, [weak_core, weak_run](bool observed) {
            const std::shared_ptr<Core> core = weak_core.lock();
            const std::shared_ptr<Run> ended = weak_run.lock();
            if (core && ended) core->on_exit(ended, observed);
        });
        if (auto* problem = std::get_if<std::string>(&started)) return std::move(*problem);
        run->child = std::get<std::unique_ptr<Child>>(std::move(started));
        run->pid = run->child->pid();
        run->error_ended = run->child->error() == nullptr;
        run_ = run;
        report(*run, ChildProcessStep::started, cause);
        read_output(run);
        if (!run->error_ended) read_error(run);
        return std::nullopt;
    }

    std::size_t error_line_bound() const noexcept {
        return process_.max_stderr_line_bytes == 0 ? ChildProcessDefaults::max_stderr_line_bytes
                                                   : process_.max_stderr_line_bytes;
    }

    // Posted: the client asked for a new process after the last one ended.
    void restart() {
        if (closing_ || closed_) return;
        if (run_ && !run_->finished) return;
        if (spawn(ChildProcessCause::restart)) {
            // It could not be started: to the client that is a process that ended at once, which it counts.
            if (deliverable()) events_->stream_event(connection_unit, ProcessExited{});
            return;
        }
        if (deliverable()) events_->restarted();
    }

    // ---- Writing

    void write_next(const std::shared_ptr<Run>& run) {
        run->writing = true;
        asio::async_write(run->child->input(), asio::buffer(run->writes.front()),
                          [self = shared_from_this(), run](const std::error_code& error, std::size_t) {
                              self->on_written(run, error);
                          });
    }

    void on_written(const std::shared_ptr<Run>& run, const std::error_code& error) {
        if (!run->writes.empty()) run->writes.pop_front();
        if (error) {
            run->writing = false;
            run->writes.clear();
            // A process whose input cannot be written to can no longer be spoken to.
            if (!closing_ && !closed_ && run->input_open && !run->exited) end_now(run, ChildProcessCause::input_failed);
            return;
        }
        if (run->writes.empty() || !run->input_open) {
            run->writing = false;
            return;
        }
        write_next(run);
    }

    // ---- Reading

    void read_output(const std::shared_ptr<Run>& run) {
        run->child->output().async_read_some(
            asio::buffer(run->output_buffer),
            [self = shared_from_this(), run](const std::error_code& error, std::size_t count) {
                self->on_output(run, error, count);
            });
    }

    void on_output(const std::shared_ptr<Run>& run, const std::error_code& error, std::size_t count) {
        if (closed_ || run->finished) return;
        if (count > 0 && !run->refused) {
            run->lines.feed(std::string_view(run->output_buffer.data(), count),
                            [&](std::string_view line) { take_line(run, line); });
        }
        if (!error) {
            if (!run->refused) read_output(run);
            return;
        }
        // The output ended: every process that held it ended, or closed it.
        run->output_ended = true;
        if (!run->refused) run->lines.finish([&](std::string_view line) { take_line(run, line); });
        if (!run->exited && !closing_) {
            // A process that closed its output and runs on cannot answer any more; one that is exiting is given the
            // time to.
            start_bound(run, process_.exit_grace);
        }
        finish_if_ready(run);
    }

    void take_line(const std::shared_ptr<Run>& run, std::string_view line) {
        if (run->refused || run != run_ || !deliverable() || closing_) return;
        switch (events_->received(connection_unit, line)) {
            case Reception::not_json:
                ++run->ignored_lines;
                run->ignored_bytes += line.size();
                break;
            case Reception::too_large:
            case Reception::too_deep:
                // Its bytes are bounded by the bound on a message and were never stored, so only the line counts.
                ++run->ignored_lines;
                break;
            case Reception::accepted:
            case Reception::not_message:
            case Reception::ignored: return;
        }
        if (run->ignored_lines > process_.max_ignored_stdout_lines ||
            run->ignored_bytes > process_.max_ignored_stdout_bytes) {
            // Nothing more of its output is read: for its end, the output ended here.
            run->refused = true;
            run->output_ended = true;
            end_now(run, ChildProcessCause::output_refused);
        }
    }

    void read_error(const std::shared_ptr<Run>& run) {
        ChildPipe* error_pipe = run->child->error();
        error_pipe->async_read_some(asio::buffer(run->error_buffer),
                                    [self = shared_from_this(), run](const std::error_code& error, std::size_t count) {
                                        self->on_error_output(run, error, count);
                                    });
    }

    void on_error_output(const std::shared_ptr<Run>& run, const std::error_code& error, std::size_t count) {
        if (closed_ || run->finished) return;
        if (count > 0) {
            const std::string_view chunk(run->error_buffer.data(), count);
            keep_tail(*run, chunk);
            run->error_lines.feed(chunk, [&](std::string_view line) { take_error_line(line); });
        }
        if (!error) {
            read_error(run);
            return;
        }
        run->error_ended = true;
        run->error_lines.finish([&](std::string_view line) { take_error_line(line); });
        finish_if_ready(run);
    }

    void keep_tail(Run& run, std::string_view chunk) const {
        const std::size_t keep = process_.stderr_tail_bytes;
        if (keep == 0) return;
        run.tail.append(chunk);
        // Trimmed once it holds twice the bound, so that the bytes are moved only now and then.
        if (run.tail.size() > keep * 2) run.tail.erase(0, run.tail.size() - keep);
    }

    std::string tail_of(const Run& run) const {
        const std::size_t keep = process_.stderr_tail_bytes;
        if (run.tail.size() <= keep) return run.tail;
        return run.tail.substr(run.tail.size() - keep);
    }

    void take_error_line(std::string_view line) const {
        if (!process_.on_stderr_line || closed_) return;
        const std::string_view cut = line.substr(0, process_.max_stderr_line_bytes);
        try {
            process_.on_stderr_line(cut);
        } catch (...) {
        }
    }

    // ---- The end of a process

    void on_exit(const std::shared_ptr<Run>& run, bool observed) {
        if (run->exited) return;
        run->exited = true;
        run->observed = observed;
        close_input(run, ChildProcessCause::process_ended);
        // The leader is a zombie until it is collected, so the number of its group still names its group: what is left
        // of the group is ended before the leader is collected.
        if (observed && !run->kill_sent) kill(run, ChildProcessCause::process_ended);
        run->how = run->child->collect(observed);
        run->collected = true;
        run->wake.cancel();
        if (!closing_ && !(run->output_ended && run->error_ended)) start_bound(run, process_.exit_grace);
        finish_if_ready(run);
    }

    void start_bound(const std::shared_ptr<Run>& run, std::chrono::milliseconds duration) {
        if (run->bound_running) run->bound.cancel();
        run->bound_running = true;
        run->bound.expires_after(duration);
        run->bound.async_wait([self = shared_from_this(), run](const std::error_code& error) {
            if (error) return;
            run->bound_running = false;
            self->on_bound(run);
        });
    }

    void on_bound(const std::shared_ptr<Run>& run) {
        if (closed_ || run->finished) return;
        if (!run->exited) {
            // Its output ended and it did not exit.
            end_now(run, ChildProcessCause::output_closed);
            return;
        }
        run->bound_lapsed = true;
        finish_if_ready(run);
    }

    void finish_if_ready(const std::shared_ptr<Run>& run) {
        if (run->finished || !run->collected) return;
        if (!(run->output_ended && run->error_ended) && !run->bound_lapsed && !closing_) return;
        run->finished = true;
        if (run->bound_running) {
            run->bound_running = false;
            run->bound.cancel();
        }
        std::error_code ignored;
        run->child->output().close(ignored);
        if (ChildPipe* error_pipe = run->child->error()) error_pipe->close(ignored);
        report(*run, ChildProcessStep::exited, ChildProcessCause::process_ended, run->how, tail_of(*run));
        run->wake.cancel();
        if (!closing_ && run == run_ && deliverable()) {
            events_->stream_event(connection_unit, ProcessExited{run->how.exit_code});
        }
    }

    // ---- Steps

    void close_input(const std::shared_ptr<Run>& run, ChildProcessCause cause) {
        if (!run->input_open) return;
        run->input_open = false;
        run->writes.clear();
        std::error_code ignored;
        run->child->input().close(ignored);
        report(*run, ChildProcessStep::input_closed, cause);
    }

    void terminate(const std::shared_ptr<Run>& run, ChildProcessCause cause) {
        if (run->exited || run->terminate_sent) return;
        run->terminate_sent = true;
        run->child->terminate();
        report(*run, ChildProcessStep::terminate_sent, cause);
    }

    void kill(const std::shared_ptr<Run>& run, ChildProcessCause cause) {
        if (run->kill_sent || run->collected) return;
        run->kill_sent = true;
        run->child->kill();
        report(*run, ChildProcessStep::kill_sent, cause);
    }

    // A process that cannot go on: its input is closed and its group ended at once.
    void end_now(const std::shared_ptr<Run>& run, ChildProcessCause cause) {
        close_input(run, cause);
        if (!run->exited) kill(run, cause);
    }

    // Waits `duration`, or less when the process exits first.
    static asio::awaitable<void> wait(std::shared_ptr<Run> run, std::chrono::milliseconds duration) {
        if (run->exited) co_return;
        run->wake.expires_after(duration);
        std::error_code ignored;
        co_await run->wake.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
    }

    static asio::awaitable<void> wait_until_finished(std::shared_ptr<Run> run) {
        if (run->finished) co_return;
        run->wake.expires_at(asio::steady_timer::time_point::max());
        std::error_code ignored;
        co_await run->wake.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
    }

    // ---- Telling

    bool deliverable() const noexcept { return events_ != nullptr && !closed_; }

    void report(const Run& run, ChildProcessStep step, ChildProcessCause cause, const ChildExit& how = {},
                std::string tail = {}) const {
        if (!options_.observer) return;
        ChildProcessEvent event;
        event.step = step;
        event.cause = cause;
        event.pid = run.pid;
        event.exit_code = how.exit_code;
        event.signal = how.signal;
        event.stderr_tail = std::move(tail);
        try {
            options_.observer(event);
        } catch (...) {
        }
    }

    ChildProcess process_;
    ChildProcessTransportOptions options_;
    std::vector<std::string> inherited_;
    TransportEvents* events_ = nullptr;
    asio::any_io_executor executor_;
    std::shared_ptr<Run> run_;
    bool closing_ = false;
    bool closed_ = false;
    std::optional<SigpipeGuard> sigpipe_;
};

class ChildProcessTransport final : public Transport {
public:
    explicit ChildProcessTransport(std::shared_ptr<Core> core) : core_(std::move(core)) {}
    ChildProcessTransport(const ChildProcessTransport&) = delete;
    ChildProcessTransport& operator=(const ChildProcessTransport&) = delete;
    ~ChildProcessTransport() override {
        if (started_ && !closed_) core_->abandon();
    }

    profile::Transport kind() const noexcept override { return profile::Transport::stdio; }

    std::optional<std::size_t> max_message_bytes() const noexcept override { return core_->max_message_bytes(); }

    std::optional<std::string> start(TransportEvents& events) override {
        if (started_) return std::string("the transport was started twice");
        started_ = true;
        return core_->start(events);
    }

    void send(Outgoing outgoing) override { core_->send(std::move(outgoing)); }

    asio::awaitable<void> close(CloseCause cause) override {
        closed_ = true;
        return core_->close(cause);
    }

private:
    std::shared_ptr<Core> core_;
    bool started_ = false;
    bool closed_ = false;
};

}  // namespace

Result<std::unique_ptr<Transport>> make_child_transport(ChildProcess process, ChildProcessTransportOptions options) {
    if (auto problem = check(process, options)) return invalid(std::move(*problem));
    auto core = std::make_shared<Core>(std::move(process), std::move(options));
    return std::unique_ptr<Transport>(std::make_unique<ChildProcessTransport>(std::move(core)));
}

}  // namespace detail

Result<std::unique_ptr<Transport>> make_transport(ChildProcess process, ChildProcessTransportOptions options) {
    return detail::make_child_transport(std::move(process), std::move(options));
}

Result<std::unique_ptr<Transport>> make_transport(ChildProcess process) {
    return detail::make_child_transport(std::move(process), ChildProcessTransportOptions{});
}

Result<Client> Client::open(ClientOptions options, ChildProcess process) {
    ChildProcessTransportOptions transport_options;
    transport_options.max_message_bytes = options.max_message_bytes;
    auto transport = detail::make_child_transport(std::move(process), std::move(transport_options));
    if (!transport) return std::move(transport).error();
    return open(std::move(options), std::move(*transport));
}

asio::awaitable<Result<Client>> Client::async_open(ClientOptions options, ChildProcess process) {
    ChildProcessTransportOptions transport_options;
    transport_options.max_message_bytes = options.max_message_bytes;
    auto transport = detail::make_child_transport(std::move(process), std::move(transport_options));
    if (!transport) return failed_open(std::move(transport).error());
    return async_open(std::move(options), std::move(*transport));
}

}  // namespace neomcp
