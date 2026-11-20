// Shared by the tests of the child-process transport: the programs they start, scripts for the scripted server, a
// folder of their own per test, bounded waits, a recorder of the transport's steps, and a rig that plays the client
// to a transport on a thread of its own.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/use_future.hpp>

#include "neomcp/client.h"
#include "neomcp/client/stdio.h"
#include "neomcp/json.h"

namespace client_stdio_test {

using neomcp::json;

inline constexpr auto wait_bound = std::chrono::seconds(20);
// A grace the test expects never to run out: the step it guards is not reached.
inline constexpr std::chrono::milliseconds long_grace{20'000};
// A grace the test expects to run out: the step after it is the one under test.
inline constexpr std::chrono::milliseconds short_grace{50};

inline const std::string scripted_server = NEOMCP_SCRIPTED_SERVER;
inline const std::string library_server = NEOMCP_STDIO_FIXTURE;
inline const std::string profile_server = NEOMCP_PROFILE_SERVER;

// Waits until `done` holds, for at most the bound; whether it held. It looks again every millisecond.
inline bool eventually(const std::function<bool()>& done, std::chrono::milliseconds bound = wait_bound) {
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) return done();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

inline std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// A folder for one test, removed with what it holds when the test ends.
class Folder {
public:
    Folder() {
        std::random_device seed;
        const auto name = "neomcp-client-stdio-" + std::to_string(seed()) + "-" + std::to_string(seed());
        path_ = std::filesystem::temp_directory_path() / name;
        std::filesystem::create_directories(path_);
    }
    Folder(const Folder&) = delete;
    Folder& operator=(const Folder&) = delete;
    ~Folder() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    const std::filesystem::path& path() const noexcept { return path_; }
    std::string file(std::string_view name) const { return (path_ / std::string(name)).string(); }

private:
    std::filesystem::path path_;
};

// Writes a script for the scripted server into the folder and describes the process that runs it.
inline neomcp::ChildProcess scripted(const Folder& folder, const json& script, std::vector<std::string> arguments = {}) {
    const std::string path = folder.file("script-" + std::to_string(std::hash<std::string>{}(script.dump())) + ".json");
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << script.dump();
    }
    neomcp::ChildProcess process;
    process.program = scripted_server;
    process.arguments.push_back(path);
    for (auto& argument : arguments) process.arguments.push_back(std::move(argument));
    return process;
}

// A file the scripted server writes whole (it renames it into place), read once it exists.
inline std::optional<json> await_json(const std::string& path) {
    if (!eventually([&] { return std::filesystem::exists(path); })) return std::nullopt;
    json value = json::parse(read_file(path), nullptr, false);
    if (value.is_discarded()) return std::nullopt;
    return value;
}

// Records the steps a transport reports, from the client's thread, for the test's thread.
class Steps {
public:
    neomcp::ChildProcessObserver observer() {
        return [this](const neomcp::ChildProcessEvent& event) {
            std::lock_guard lock(mutex_);
            events_.push_back(event);
        };
    }
    std::vector<neomcp::ChildProcessEvent> all() const {
        std::lock_guard lock(mutex_);
        return events_;
    }
    std::vector<neomcp::ChildProcessStep> steps() const {
        std::vector<neomcp::ChildProcessStep> found;
        for (const auto& event : all()) found.push_back(event.step);
        return found;
    }
    std::size_t count(neomcp::ChildProcessStep step) const {
        std::size_t found = 0;
        for (const auto& event : all()) found += event.step == step ? 1 : 0;
        return found;
    }
    // The last event of a step, once there is one.
    std::optional<neomcp::ChildProcessEvent> await(neomcp::ChildProcessStep step, std::size_t nth = 1) const {
        std::optional<neomcp::ChildProcessEvent> found;
        eventually([&] {
            std::size_t seen = 0;
            for (const auto& event : all()) {
                if (event.step == step && ++seen == nth) {
                    found = event;
                    return true;
                }
            }
            return false;
        });
        return found;
    }

private:
    mutable std::mutex mutex_;
    std::vector<neomcp::ChildProcessEvent> events_;
};

// Plays the client to one transport: its events run on a thread of the rig, as a client's do, and the test calls the
// transport there through start(), send() and close().
class Rig final : public neomcp::TransportEvents {
public:
    Rig() : guard_(asio::make_work_guard(io_)), thread_([this] { io_.run(); }) {}
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;
    ~Rig() override {
        guard_.reset();
        if (thread_.joinable()) thread_.join();
    }

    asio::any_io_executor executor() const noexcept override { return io_.get_executor(); }

    neomcp::Reception received(neomcp::UnitId, std::string_view message) override {
        std::lock_guard lock(mutex_);
        lines_.emplace_back(message);
        order_.push_back("line");
        if (message.size() > max_message_bytes) return neomcp::Reception::too_large;
        const json value = json::parse(message, nullptr, false);
        return value.is_discarded() ? neomcp::Reception::not_json : neomcp::Reception::accepted;
    }

    void stream_event(neomcp::UnitId, neomcp::StreamEvent event) override {
        std::lock_guard lock(mutex_);
        if (std::holds_alternative<neomcp::ProcessExited>(event)) order_.push_back("exited");
        if (std::holds_alternative<neomcp::SendFailed>(event)) order_.push_back("send_failed");
        events_.push_back(std::move(event));
    }

    void restarted() override {
        std::lock_guard lock(mutex_);
        order_.push_back("restarted");
        ++restarts_;
    }

    // ---- The test's side

    // Runs `work` on the rig's thread and returns what it returns.
    template <class F>
    auto on_io(F work) -> decltype(work()) {
        std::packaged_task<decltype(work())()> task(std::move(work));
        auto result = task.get_future();
        asio::post(io_, [&task] { task(); });
        return result.get();
    }

    std::optional<std::string> start(neomcp::Transport& transport) {
        return on_io([&] { return transport.start(*this); });
    }

    void send(neomcp::Transport& transport, std::string line,
              neomcp::OutgoingKind kind = neomcp::OutgoingKind::request) {
        on_io([&] {
            neomcp::Outgoing outgoing;
            outgoing.line = std::move(line);
            outgoing.kind = kind;
            transport.send(std::move(outgoing));
            return 0;
        });
    }

    // Holds the rig's thread until the returned promise is set (or let go), so that what the transport's operations
    // complete meanwhile waits in its queue.
    std::shared_ptr<std::promise<void>> hold() {
        auto release = std::make_shared<std::promise<void>>();
        auto held = std::make_shared<std::promise<void>>();
        asio::post(io_, [waited = release->get_future().share(), held] {
            held->set_value();
            static_cast<void>(waited.wait_for(wait_bound));
        });
        held->get_future().wait();
        return release;
    }

    // Whether the close completed within the bound. When it did not, the transport is still closing on the rig's
    // thread and must not be destroyed: the caller ends the test program.
    bool close(neomcp::Transport& transport, neomcp::CloseCause cause = neomcp::CloseCause::client_closed) {
        auto closed = asio::co_spawn(io_, transport.close(cause), asio::use_future);
        if (closed.wait_for(wait_bound) != std::future_status::ready) return false;
        closed.get();
        return true;
    }

    std::vector<std::string> lines() const {
        std::lock_guard lock(mutex_);
        return lines_;
    }
    std::vector<std::string> order() const {
        std::lock_guard lock(mutex_);
        return order_;
    }
    std::size_t restarts() const {
        std::lock_guard lock(mutex_);
        return restarts_;
    }
    // The ProcessExited events delivered so far.
    std::vector<neomcp::ProcessExited> exits() const {
        std::lock_guard lock(mutex_);
        std::vector<neomcp::ProcessExited> found;
        for (const auto& event : events_) {
            if (auto* exited = std::get_if<neomcp::ProcessExited>(&event)) found.push_back(*exited);
        }
        return found;
    }
    std::size_t send_failures() const {
        std::lock_guard lock(mutex_);
        std::size_t found = 0;
        for (const auto& event : events_) found += std::holds_alternative<neomcp::SendFailed>(event) ? 1 : 0;
        return found;
    }

    std::size_t max_message_bytes = neomcp::ClientDefaults::max_message_bytes;

private:
    mutable asio::io_context io_;
    asio::executor_work_guard<asio::io_context::executor_type> guard_;
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
    std::vector<std::string> order_;
    std::vector<neomcp::StreamEvent> events_;
    std::size_t restarts_ = 0;
    std::thread thread_;
};

// Makes a transport with an observer, or fails the test.
inline std::unique_ptr<neomcp::Transport> transport_of(neomcp::ChildProcess process, Steps& steps,
                                                       std::size_t max_message_bytes =
                                                           neomcp::ClientDefaults::max_message_bytes) {
    neomcp::ChildProcessTransportOptions options;
    options.observer = steps.observer();
    options.max_message_bytes = max_message_bytes;
    auto made = neomcp::make_transport(std::move(process), std::move(options));
    if (!made) return nullptr;
    return std::move(*made);
}

// Options that enable exactly these revisions.
inline neomcp::ClientOptions only(std::initializer_list<std::string_view> revisions) {
    neomcp::ClientOptions options;
    for (const auto revision : revisions) options.profiles.push_back(neomcp::profile::builtin(revision));
    options.client_info.name = "client-stdio-test";
    options.client_info.version = "1.0.0";
    return options;
}

inline neomcp::ClientOptions every_revision() {
    neomcp::ClientOptions options;
    options.client_info.name = "client-stdio-test";
    options.client_info.version = "1.0.0";
    return options;
}

// A handshake result at `revision`, and a discovery result that names it.
inline json initialize_result(std::string_view revision) {
    return json{{"protocolVersion", std::string(revision)},
                {"capabilities", json::object()},
                {"serverInfo", json{{"name", "scripted"}, {"version", "1"}}}};
}

inline json discover_result(std::string_view revision) {
    json list = json::array();
    list.push_back(std::string(revision));
    return json{{"supportedVersions", list}, {"capabilities", json::object()}, {"ttlMs", 0},
                {"cacheScope", "private"}, {"resultType", "complete"}};
}

inline json action(std::string_view kind, json value) {
    json one = json::object();
    one[std::string(kind)] = std::move(value);
    return one;
}

inline json rule(std::string_view method, json actions) {
    return json{{"method", std::string(method)}, {"actions", std::move(actions)}};
}

inline json tool_rule(std::string_view name, json actions) {
    return json{{"method", "tools/call"}, {"name", std::string(name)}, {"actions", std::move(actions)}};
}

}  // namespace client_stdio_test
