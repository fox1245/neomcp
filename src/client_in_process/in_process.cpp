// A client connected to a Server in the same process (neomcp/client/in_process.h).
//
// A session is fed by one thread at a time, so one mutex covers every call into it: the client's thread feeds it and
// a test may end it from another thread. The session writes through the link, which takes only the link's own lock,
// so holding this mutex while the session writes adds no cycle.
#include "neomcp/client/in_process.h"

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace neomcp {

namespace detail {

class InProcessServerCore {
public:
    InProcessServerCore(Server& server, profile::Transport transport_kind) : server_(server), kind_(transport_kind) {}

    ~InProcessServerCore() {
        std::lock_guard lock(mutex_);
        end_session();
    }

    bool take_transport() {
        std::lock_guard lock(mutex_);
        if (transport_taken_) return false;
        transport_taken_ = true;
        return true;
    }

    profile::Transport kind() const noexcept { return kind_; }

    std::optional<std::string> attach(MemoryLink link) {
        std::lock_guard lock(mutex_);
        link_ = std::move(link);
        detached_ = false;
        return start_process();
    }

    void receive(Outgoing outgoing) {
        std::lock_guard lock(mutex_);
        switch (outgoing.kind) {
            case OutgoingKind::request:
            case OutgoingKind::notification:
            case OutgoingKind::response: feed(outgoing); return;
            case OutgoingKind::end_unit:
                if (session_ && kind_ != profile::Transport::stdio) session_->abandon(outgoing.unit);
                return;
            case OutgoingKind::restart: restart(); return;
            case OutgoingKind::open_connection_stream:
            case OutgoingKind::close: return;
        }
    }

    void detach() {
        std::lock_guard lock(mutex_);
        end_session();
        detached_ = true;
        link_.reset();
    }

    void end_process(std::optional<int> status) {
        if (kind_ != profile::Transport::stdio) return;
        std::lock_guard lock(mutex_);
        if (detached_ || !session_ || !link_) return;
        end_session();
        link_->event(connection_unit, ProcessExited{status});
    }

    std::size_t processes() const noexcept { return processes_.load(); }

private:
    // Under the mutex, as every function below.
    void feed(const Outgoing& outgoing) {
        if (!session_) return;
        if (kind_ == profile::Transport::stdio) {
            session_->receive(outgoing.line);
            return;
        }
        std::vector<HeaderField> fields;
        fields.reserve(outgoing.headers.size());
        for (const auto& header : outgoing.headers) fields.push_back({header.name, header.value});
        session_->receive(outgoing.unit, outgoing.line, fields);
    }

    // A new server process: a new session of the server, which writes to the client through the link.
    std::optional<std::string> start_process() {
        if (detached_ || !link_) return std::string("the in-process server is not attached");
        std::variant<std::unique_ptr<Session>, CreateError> connected = connect(*link_);
        if (auto* error = std::get_if<CreateError>(&connected)) return error->message;
        session_ = std::move(std::get<std::unique_ptr<Session>>(connected));
        ++processes_;
        return std::nullopt;
    }

    std::variant<std::unique_ptr<Session>, CreateError> connect(const MemoryLink& link) {
        if (kind_ == profile::Transport::stdio) {
            return server_.connect([link](std::string line) { link.message(connection_unit, std::move(line)); });
        }
        return server_.connect_tagged(kind_, [link](Output output) {
            const UnitId unit = output.unit.value_or(connection_unit);
            const bool end = output.kind == OutputKind::end;
            if (!end) link.message(unit, std::move(output.line));
            if (end || output.last) link.event(unit, StreamEnded{});
        });
    }

    void restart() {
        end_session();
        if (start_process().has_value()) return;
        if (link_) link_->restarted();
    }

    void end_session() {
        if (!session_) return;
        std::unique_ptr<Session> ended = std::move(session_);
        ended->close();
    }

    Server& server_;
    profile::Transport kind_;
    mutable std::mutex mutex_;
    std::optional<MemoryLink> link_;
    std::unique_ptr<Session> session_;
    std::atomic<std::size_t> processes_{0};
    bool transport_taken_ = false;
    bool detached_ = false;
};

}  // namespace detail

InProcessServer::InProcessServer(Server& server, profile::Transport kind)
    : core_(std::make_unique<detail::InProcessServerCore>(server, kind)) {}

InProcessServer::~InProcessServer() = default;

std::shared_ptr<InProcessServer> InProcessServer::create(Server& server, profile::Transport kind) {
    return std::shared_ptr<InProcessServer>(new InProcessServer(server, kind));
}

std::unique_ptr<Transport> InProcessServer::transport() {
    if (!core_->take_transport()) throw std::logic_error("the in-process server's transport was taken already");
    return make_memory_transport(core_->kind(), shared_from_this());
}

void InProcessServer::end_process(std::optional<int> status) { core_->end_process(status); }

std::size_t InProcessServer::processes() const noexcept { return core_->processes(); }

std::optional<std::string> InProcessServer::attach(MemoryLink link) { return core_->attach(std::move(link)); }

void InProcessServer::receive(Outgoing outgoing) { core_->receive(std::move(outgoing)); }

void InProcessServer::detach(CloseCause) { core_->detach(); }

}  // namespace neomcp
