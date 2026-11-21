// Where the state of tasks lives: records of long-running requests that were answered with a task, which the requester
// then asks about by its id.
//
// `TaskStore` is the interface; `MemoryTaskStore` is the one the library ships. A store of an application that must
// keep tasks across a restart, or share them between server processes, implements the interface over its own storage.
//
// What a store promises, and what an implementation of it must keep:
//  - Every call may come from any thread, at any time, and no call blocks on another caller's work. A listener is
//    never called while the store holds a lock of its own, so a listener may call the store.
//  - `create` returns only once `get` of the returned id finds the record, from any thread. A store whose storage
//    becomes consistent later waits for that before it returns.
//  - A status change is one compare-and-set (`transition`): of two callers that race to end a task, one wins and the
//    other is told the status it found. A task that has ended (completed, failed or cancelled) never changes again.
//  - An id that does not exist, has expired, or belongs to another owner is answered the same way, so an answer never
//    shows whether an id exists.
//  - Ids are drawn from the operating system's random source and are not guessable: whoever holds an id may ask
//    about the task, so an id is a secret of the requester. Do not write whole ids to logs.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "neomcp/json.h"

namespace neomcp {

// Who a task belongs to. The principal of the authorization context the task was created in; empty when the request
// carried none (a server without sign-in). An empty owner and a non-empty one never match.
struct TaskOwner {
    std::string principal;

    friend bool operator==(const TaskOwner&, const TaskOwner&) = default;
};

enum class TaskStatus { working, input_required, completed, failed, cancelled };

// A task that has ended: completed, failed or cancelled. Its status never changes again.
constexpr bool is_terminal(TaskStatus status) noexcept {
    return status == TaskStatus::completed || status == TaskStatus::failed || status == TaskStatus::cancelled;
}

// The protocol's spelling of the status: working, input_required, completed, failed, cancelled.
std::string_view to_string(TaskStatus status) noexcept;

// The clock of a store: the time it stamps records with and measures lifetimes against. Empty: the system clock.
using TaskClock = std::function<std::chrono::system_clock::time_point()>;

struct TaskRecord {
    std::string id;
    TaskOwner owner;
    TaskStatus status = TaskStatus::working;
    std::optional<std::string> status_message;
    std::chrono::system_clock::time_point created{};
    std::chrono::system_clock::time_point updated{};
    // How long the record is kept, counted from `created`. A store that keeps records without limit leaves it empty;
    // MemoryTaskStore always sets it.
    std::optional<std::chrono::milliseconds> ttl;
    std::chrono::milliseconds poll_interval{0};
    std::string revision;  // the revision the task was created under
    json request = nullptr;  // what the task runs; the store keeps it and does not read it
    // How the task ended: what the request would have answered with, or the error it ended with. A completed task
    // has a result and a failed one an error; a store keeps whatever the transition gave it.
    std::optional<json> result;
    std::optional<json> error;
    // The requests for input a task in `input_required` waits on, and the answers received so far, each an object
    // by key (see put_answers). Both are empty otherwise.
    std::optional<json> asked;
    std::optional<json> answers;
};

// What a caller gives to create a task. The store draws the id and stamps the times.
struct TaskDraft {
    TaskOwner owner;
    // The lifetime asked for; empty: the store's default. The store keeps it between its smallest and largest.
    std::optional<std::chrono::milliseconds> ttl;
    std::string revision;
    json request = nullptr;
};

// Why a task was not created.
enum class CreateRefusal {
    store_full,       // the store holds as many tasks as it may
    owner_full,       // the owner holds as many tasks as one owner may
    byte_budget,      // the record would take the store past the bytes it may hold
    id_unavailable    // no id could be drawn: the random source failed or kept returning ids in use
};

using CreateResult = std::variant<TaskRecord, CreateRefusal>;

// The new state of a task. `message` replaces the task's status message (empty clears it); a field left empty
// leaves the task's value as it is.
struct TaskUpdate {
    TaskStatus status = TaskStatus::working;
    std::optional<std::string> message;
    std::optional<json> result;
    std::optional<json> error;
    std::optional<json> asked;
};

enum class TransitionOutcome {
    applied,            // the task now has the new state; `record` is it
    not_found,          // no such task (or it expired); `record` is empty
    status_mismatch,    // the task is not in one of the statuses expected, or has ended; `record` is what was found
    too_large,          // a result, error or request for input is larger than one may be; nothing changed
    over_budget         // the change would take the store past the bytes it may hold; nothing changed
};

struct Transition {
    TransitionOutcome outcome = TransitionOutcome::not_found;
    std::optional<TaskRecord> record;
};

enum class AnswersOutcome {
    applied,      // the answers for keys that were asked and not yet answered are kept; `record` is the task
    not_found,
    not_waiting,  // the task is not waiting for input (its status is not input_required); `record` is what was found
    too_large,    // the answers are larger than one may be; nothing changed
    over_budget
};

struct AnswersResult {
    AnswersOutcome outcome = AnswersOutcome::not_found;
    std::optional<TaskRecord> record;
};

// A page of the tasks of one owner, oldest first. Each record carries the fields a listing shows (everything but the
// request, the result, the error and the input) and leaves the others empty.
struct TaskPage {
    std::vector<TaskRecord> tasks;
    std::optional<std::string> next_cursor;  // empty on the last page
};

// A task the store removed because its lifetime ended, and the status it had.
struct ExpiredTask {
    std::string id;
    TaskStatus status = TaskStatus::working;
};

struct TaskStoreUsage {
    std::size_t tasks = 0;
    std::size_t bytes = 0;  // what the records take, by the store's own measure (see MemoryTaskStoreOptions)
};

// What happened to a task that is watched.
enum class TaskChangeKind {
    updated,  // the status, the message, the asked requests or the answers changed
    removed   // the task is gone: it expired
};

struct TaskChange {
    std::string id;
    TaskChangeKind kind = TaskChangeKind::updated;
    TaskStatus status = TaskStatus::working;  // the status after the change; for `removed`, the one it had
};

// Receives the changes of the tasks it watches. Called on the thread that made the change, outside the store's
// locks, after the change is visible to `get`. It must not block for long and must not throw (what it throws is
// dropped).
class TaskListener {
public:
    virtual ~TaskListener() = default;
    virtual void on_task_change(const TaskChange& change) = 0;
};

// The registration of a listener. Destroying or calling `release` ends it; a change that is being delivered at that
// moment is delivered to a listener that the store still holds alive, never to a destroyed one. Move only, and valid
// after the store is gone.
class TaskWatch {
public:
    TaskWatch() = default;
    explicit TaskWatch(std::function<void()> on_release) : release_(std::move(on_release)) {}
    TaskWatch(TaskWatch&& other) noexcept : release_(std::exchange(other.release_, nullptr)) {}
    TaskWatch& operator=(TaskWatch&& other) noexcept {
        if (this != &other) {
            release();
            release_ = std::exchange(other.release_, nullptr);
        }
        return *this;
    }
    TaskWatch(const TaskWatch&) = delete;
    TaskWatch& operator=(const TaskWatch&) = delete;
    ~TaskWatch() { release(); }

    explicit operator bool() const noexcept { return static_cast<bool>(release_); }
    void release() {
        if (release_) std::exchange(release_, nullptr)();
    }

private:
    std::function<void()> release_;
};

enum class WatchOutcome {
    watching,   // the listener is registered
    not_found,  // no such task (or it expired): nothing is registered
    limit       // the store holds as many watches as it may
};

// The result of watching: the task as it was at the moment the watch started. Every change after that moment is
// delivered, so a caller that acts on `record` and then waits for the listener misses nothing.
struct TaskWatchStart {
    WatchOutcome outcome = WatchOutcome::not_found;
    TaskWatch watch;
    std::optional<TaskRecord> record;
};

class TaskStore {
public:
    virtual ~TaskStore() = default;

    // Creates a task `working`. See the promises at the top of this file.
    virtual CreateResult create(TaskDraft draft) = 0;

    // The task; empty when there is none, it expired, or it belongs to another owner.
    virtual std::optional<TaskRecord> get(std::string_view id, const TaskOwner& owner) const = 0;

    // Gives the task a new state if its status is one of `from` and has not ended. Of two calls that race, one is
    // `applied` and the other `status_mismatch`.
    virtual Transition transition(std::string_view id, std::span<const TaskStatus> from, TaskUpdate update) = 0;

    // Keeps the answers the owner gives to a task in `input_required`: those whose key was asked and has no answer
    // yet, so a repeated or unknown key is ignored. `answers` is an object by key.
    virtual AnswersResult put_answers(std::string_view id, const TaskOwner& owner, json answers) = 0;

    // When the task is in `input_required` and every key it asked has an answer: returns the answers (an object
    // by key) once, clears the asked requests and the answers, and sets the status `working`. Empty otherwise, and
    // for every call but the first that finds the answers complete.
    virtual std::optional<json> take_answers(std::string_view id) = 0;

    // The owner's tasks, oldest first, after `cursor` (empty: from the start), at most `limit` of them. The cursor
    // names a position in the owner's order, so a page does not shift when tasks expire between calls. Empty when
    // the cursor is not one this store gave.
    virtual std::optional<TaskPage> list(const TaskOwner& owner, std::optional<std::string_view> cursor,
                                         std::size_t limit) const = 0;

    // Removes the tasks whose lifetime ended by `now`, tells their watchers, and returns them.
    virtual std::vector<ExpiredTask> expire(std::chrono::system_clock::time_point now) = 0;

    // For a store that outlives the process, called once when a server starts: every task still working or waiting
    // for input has lost the code that ran it. Gives each one `failure` and returns how many. `failure` must be a
    // terminal state with a small outcome (a message and an error that says why); throws std::invalid_argument
    // otherwise.
    virtual std::size_t recover(const TaskUpdate& failure) = 0;

    // Registers `listener` for the task of `owner`; see TaskWatchStart.
    virtual TaskWatchStart watch(std::string_view id, const TaskOwner& owner, std::weak_ptr<TaskListener> listener) = 0;

    virtual TaskStoreUsage usage() const = 0;
};

// The defaults of the bounds of MemoryTaskStore. The numbers are the library's choice, not the protocol's.
struct TaskStoreDefaults {
    static constexpr std::size_t max_tasks = 1024;
    static constexpr std::size_t max_tasks_per_owner = 64;
    static constexpr std::chrono::milliseconds default_ttl{std::chrono::hours(1)};
    static constexpr std::chrono::milliseconds min_ttl{1};
    static constexpr std::chrono::milliseconds max_ttl{std::chrono::hours(24)};
    static constexpr std::chrono::milliseconds poll_interval{std::chrono::seconds(1)};
    static constexpr std::size_t max_result_bytes = std::size_t{8} * 1024 * 1024;
    static constexpr std::size_t max_stored_bytes = std::size_t{64} * 1024 * 1024;
    static constexpr std::size_t outcome_reserve_bytes = 1024;
    static constexpr std::size_t max_page_size = 50;
    static constexpr std::size_t max_watches = 1024;
    static constexpr std::size_t id_attempts = 2;
    static constexpr std::size_t id_bytes = 16;
};

// Every size is a number of bytes of the record's serialized JSON and strings. Every option must be positive, except
// `outcome_reserve_bytes`, which may be zero; a minimum above a maximum, or a default outside them, is an error.
struct MemoryTaskStoreOptions {
    // How many tasks the store holds, and how many one owner may hold (the empty owner is one owner).
    std::size_t max_tasks = TaskStoreDefaults::max_tasks;
    std::size_t max_tasks_per_owner = TaskStoreDefaults::max_tasks_per_owner;
    // The lifetime of a task: `default_ttl` when none is asked for, otherwise what is asked for kept between
    // `min_ttl` and `max_ttl`. A task is gone from the moment `ttl` has passed since it was created.
    std::chrono::milliseconds default_ttl = TaskStoreDefaults::default_ttl;
    std::chrono::milliseconds min_ttl = TaskStoreDefaults::min_ttl;
    std::chrono::milliseconds max_ttl = TaskStoreDefaults::max_ttl;
    // The interval a requester is told to poll at; kept in the record.
    std::chrono::milliseconds poll_interval = TaskStoreDefaults::poll_interval;
    // The largest result, error, set of asked requests or set of answers (each serialized).
    std::size_t max_result_bytes = TaskStoreDefaults::max_result_bytes;
    // The most bytes all records together may take. A running task counts `outcome_reserve_bytes` more than it
    // holds, so that ending it with a small outcome (an error that says why) always fits; any change that does not
    // make a record larger is accepted.
    std::size_t max_stored_bytes = TaskStoreDefaults::max_stored_bytes;
    std::size_t outcome_reserve_bytes = TaskStoreDefaults::outcome_reserve_bytes;
    // The most records a listing returns.
    std::size_t max_page_size = TaskStoreDefaults::max_page_size;
    // The most listeners registered at once.
    std::size_t max_watches = TaskStoreDefaults::max_watches;
    // How many ids are drawn for one task before it is refused: a drawn id that is in use is drawn again.
    std::size_t id_attempts = TaskStoreDefaults::id_attempts;
    // The clock; empty: the system clock.
    TaskClock clock{};
    // Where ids come from; empty: the operating system's random source (neomcp::random_hex of
    // TaskStoreDefaults::id_bytes bytes). A source that returns nothing is a failed draw. For tests of the store; an
    // id that is guessable defeats the point of the id.
    std::function<std::optional<std::string>()> new_id{};
};

// The store in the memory of the process: a restart forgets every task. Throws std::invalid_argument for an option
// that is zero or inconsistent.
std::shared_ptr<TaskStore> make_memory_task_store(MemoryTaskStoreOptions options = {});

}  // namespace neomcp
