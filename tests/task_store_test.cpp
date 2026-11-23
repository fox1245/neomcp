// The task store: records of long-running requests, their lifetime, their bounds, and the races a store must settle.
// Everything here runs against the store alone, with a clock the test moves.
#include "neomcp/server/task_store.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define NEOMCP_TASK_TEST_SANITIZER_HEAP 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define NEOMCP_TASK_TEST_SANITIZER_HEAP 1
#endif
#endif

#if defined(NEOMCP_TASK_TEST_SANITIZER_HEAP)
extern "C" std::size_t __sanitizer_get_current_allocated_bytes();
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
#include <malloc.h>
#define NEOMCP_TASK_TEST_MALLINFO2 1
#endif

namespace {

using namespace neomcp;
using Millis = std::chrono::milliseconds;
using TimePoint = std::chrono::system_clock::time_point;

// A clock the test moves. It is read from several threads, so it is atomic.
class FakeClock {
public:
    TimePoint now() const { return TimePoint(Millis(millis_.load())); }
    void advance(Millis by) { millis_ += by.count(); }
    TaskClock function() {
        return [this] { return now(); };
    }

private:
    std::atomic<std::int64_t> millis_{1'000'000'000};
};

MemoryTaskStoreOptions options_with(FakeClock& ticks) {
    MemoryTaskStoreOptions options;
    options.clock = ticks.function();
    return options;
}

TaskDraft draft(std::string principal = "", std::optional<Millis> ttl = std::nullopt) {
    TaskDraft result;
    result.owner = TaskOwner{std::move(principal)};
    result.ttl = ttl;
    result.revision = "test-revision";
    result.request = json{{"name", "tool"}, {"arguments", json{{"x", 1}}}};
    return result;
}

// Creates a task that is expected to be created.
TaskRecord make_task(TaskStore& store, TaskDraft task = draft()) {
    CreateResult result = store.create(std::move(task));
    auto* record = std::get_if<TaskRecord>(&result);
    EXPECT_NE(record, nullptr) << "the task was refused";
    return record != nullptr ? *record : TaskRecord{};
}

CreateRefusal refusal_of(TaskStore& store, TaskDraft task = draft()) {
    CreateResult result = store.create(std::move(task));
    auto* refusal = std::get_if<CreateRefusal>(&result);
    EXPECT_NE(refusal, nullptr) << "the task was created";
    return refusal != nullptr ? *refusal : CreateRefusal::store_full;
}

TaskUpdate to(TaskStatus status) {
    TaskUpdate update;
    update.status = status;
    return update;
}

Transition step(TaskStore& store, const std::string& id, TaskStatus from, TaskUpdate update) {
    const TaskStatus expected[] = {from};
    return store.transition(id, expected, std::move(update));
}

// A string value whose serialized form is exactly `bytes` bytes (the two quotes included).
json text_of_size(std::size_t bytes) {
    json value = std::string(bytes - 2, 'x');
    EXPECT_EQ(value.dump().size(), bytes);
    return value;
}

// All `parties` threads wait here until the last arrives; then all go on. It can be used round after round.
class Rendezvous {
public:
    explicit Rendezvous(std::size_t parties) : parties_(parties) {}
    void arrive_and_wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        const std::size_t generation = generation_;
        if (++arrived_ == parties_) {
            arrived_ = 0;
            ++generation_;
            changed_.notify_all();
            return;
        }
        changed_.wait(lock, [&] { return generation_ != generation; });
    }

private:
    const std::size_t parties_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::size_t arrived_ = 0;
    std::size_t generation_ = 0;
};

class Recorder final : public TaskListener {
public:
    void on_task_change(const TaskChange& change) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        changes_.push_back(change);
    }
    std::vector<TaskChange> changes() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return changes_;
    }
    std::size_t count(TaskChangeKind kind) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<std::size_t>(std::count_if(changes_.begin(), changes_.end(),
                                                      [kind](const TaskChange& change) { return change.kind == kind; }));
    }

private:
    mutable std::mutex mutex_;
    std::vector<TaskChange> changes_;
};

constexpr TaskStatus all_statuses[] = {TaskStatus::working, TaskStatus::input_required, TaskStatus::completed,
                                       TaskStatus::failed, TaskStatus::cancelled};

// ---------------------------------------------------------------------------------------------------------------
// Creating and reading

TEST(TaskStoreCreate, ANewTaskIsWorkingAndReadable) {
    FakeClock ticks;
    auto store = make_memory_task_store(options_with(ticks));
    const TaskRecord record = make_task(*store, draft("alice"));
    EXPECT_EQ(record.status, TaskStatus::working);
    EXPECT_EQ(record.owner.principal, "alice");
    EXPECT_EQ(record.revision, "test-revision");
    EXPECT_EQ(record.created, ticks.now());
    EXPECT_EQ(record.updated, ticks.now());
    EXPECT_EQ(record.ttl, std::optional<Millis>(TaskStoreDefaults::default_ttl));
    EXPECT_EQ(record.poll_interval, TaskStoreDefaults::poll_interval);
    EXPECT_EQ(record.request, draft().request);
    EXPECT_FALSE(record.result.has_value());
    EXPECT_FALSE(record.error.has_value());

    const auto found = store->get(record.id, TaskOwner{"alice"});
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->id, record.id);
    EXPECT_EQ(found->request, record.request);
    EXPECT_FALSE(store->get("no-such-task", TaskOwner{"alice"}).has_value());
}

TEST(TaskStoreCreate, StatusNamesAreTheProtocolsSpelling) {
    EXPECT_EQ(to_string(TaskStatus::working), "working");
    EXPECT_EQ(to_string(TaskStatus::input_required), "input_required");
    EXPECT_EQ(to_string(TaskStatus::completed), "completed");
    EXPECT_EQ(to_string(TaskStatus::failed), "failed");
    EXPECT_EQ(to_string(TaskStatus::cancelled), "cancelled");
    EXPECT_FALSE(is_terminal(TaskStatus::working));
    EXPECT_FALSE(is_terminal(TaskStatus::input_required));
    EXPECT_TRUE(is_terminal(TaskStatus::completed));
    EXPECT_TRUE(is_terminal(TaskStatus::failed));
    EXPECT_TRUE(is_terminal(TaskStatus::cancelled));
}

TEST(TaskStoreIds, AreRandomHexOfTheDefaultLength) {
    // An id from a counter or a clock fails the spread of every position over many ids: the leading positions of a
    // counter never change.
    MemoryTaskStoreOptions options;
    options.max_tasks = 400;
    options.max_tasks_per_owner = 400;
    auto store = make_memory_task_store(options);
    std::set<std::string> ids;
    std::vector<std::set<char>> seen(TaskStoreDefaults::id_bytes * 2);
    constexpr std::size_t count = 256;
    for (std::size_t i = 0; i < count; ++i) {
        const TaskRecord record = make_task(*store);
        ASSERT_EQ(record.id.size(), TaskStoreDefaults::id_bytes * 2);
        for (std::size_t position = 0; position < record.id.size(); ++position) {
            const char c = record.id[position];
            EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << record.id;
            seen[position].insert(c);
        }
        ids.insert(record.id);
    }
    EXPECT_EQ(ids.size(), count);
    for (std::size_t position = 0; position < seen.size(); ++position) {
        EXPECT_GE(seen[position].size(), 8u) << "position " << position << " of the ids hardly varies";
    }
}

TEST(TaskStoreIds, ADrawnIdThatIsInUseIsDrawnAgain) {
    FakeClock ticks;
    MemoryTaskStoreOptions options = options_with(ticks);
    std::vector<std::string> draws{"same", "same", "same", "other"};
    std::size_t next = 0;
    options.new_id = [&]() -> std::optional<std::string> { return draws[std::min(next++, draws.size() - 1)]; };
    options.id_attempts = 2;
    auto store = make_memory_task_store(options);
    EXPECT_EQ(make_task(*store).id, "same");       // draw 1
    EXPECT_EQ(refusal_of(*store), CreateRefusal::id_unavailable);  // draws 2 and 3 are both in use
    EXPECT_EQ(make_task(*store).id, "other");      // draw 4
}

TEST(TaskStoreIds, AFailedOrEmptyDrawRefusesTheTask) {
    FakeClock ticks;
    MemoryTaskStoreOptions options = options_with(ticks);
    bool fail = true;
    options.new_id = [&]() -> std::optional<std::string> {
        if (fail) return std::nullopt;
        return std::string();
    };
    auto store = make_memory_task_store(options);
    EXPECT_EQ(refusal_of(*store), CreateRefusal::id_unavailable);
    fail = false;
    EXPECT_EQ(refusal_of(*store), CreateRefusal::id_unavailable) << "an empty id is no id";
    EXPECT_EQ(store->usage().tasks, 0u);
}

TEST(TaskStoreTtl, TheDefaultAndTheClampedRequest) {
    FakeClock ticks;
    MemoryTaskStoreOptions options = options_with(ticks);
    options.default_ttl = Millis(5000);
    options.min_ttl = Millis(100);
    options.max_ttl = Millis(10'000);
    auto store = make_memory_task_store(options);
    const auto ttl_of = [&](std::optional<Millis> asked) {
        return make_task(*store, draft("", asked)).ttl;
    };
    EXPECT_EQ(ttl_of(std::nullopt), std::optional<Millis>(5000));
    EXPECT_EQ(ttl_of(Millis(7000)), std::optional<Millis>(7000));
    EXPECT_EQ(ttl_of(Millis(100)), std::optional<Millis>(100));
    EXPECT_EQ(ttl_of(Millis(99)), std::optional<Millis>(100));
    EXPECT_EQ(ttl_of(Millis(0)), std::optional<Millis>(100));
    EXPECT_EQ(ttl_of(Millis(-5)), std::optional<Millis>(100));
    EXPECT_EQ(ttl_of(Millis(10'000)), std::optional<Millis>(10'000));
    EXPECT_EQ(ttl_of(Millis(10'001)), std::optional<Millis>(10'000));
    EXPECT_EQ(ttl_of((Millis::max)()), std::optional<Millis>(10'000)) << "unlimited is never granted";
}

// ---------------------------------------------------------------------------------------------------------------
// Expiry

TEST(TaskStoreExpiry, ATaskIsKeptForItsTtlAndGoneAtItsEnd) {
    FakeClock ticks;
    auto store = make_memory_task_store(options_with(ticks));
    const TaskRecord record = make_task(*store, draft("", Millis(1000)));
    const TaskOwner owner;
    ticks.advance(Millis(999));
    EXPECT_TRUE(store->get(record.id, owner).has_value()) << "at ttl - 1 ms";
    ticks.advance(Millis(1));
    EXPECT_FALSE(store->get(record.id, owner).has_value()) << "at ttl";
    ticks.advance(Millis(1));
    EXPECT_FALSE(store->get(record.id, owner).has_value()) << "at ttl + 1 ms";
}

TEST(TaskStoreExpiry, ExpireRemovesWhatEndedAndReportsItsStatus) {
    FakeClock ticks;
    auto store = make_memory_task_store(options_with(ticks));
    const TaskRecord running = make_task(*store, draft("", Millis(1000)));
    const TaskRecord finished = make_task(*store, draft("", Millis(1000)));
    const TaskRecord later = make_task(*store, draft("", Millis(5000)));
    ASSERT_EQ(step(*store, finished.id, TaskStatus::working, to(TaskStatus::completed)).outcome,
              TransitionOutcome::applied);
    EXPECT_EQ(store->usage().tasks, 3u);

    ticks.advance(Millis(999));
    EXPECT_TRUE(store->expire(ticks.now()).empty()) << "nothing has ended at ttl - 1 ms";
    EXPECT_EQ(store->usage().tasks, 3u);
    EXPECT_TRUE(store->get(finished.id, TaskOwner{}).has_value()) << "an ended task is kept until its ttl";

    ticks.advance(Millis(1));
    std::vector<ExpiredTask> gone = store->expire(ticks.now());
    std::sort(gone.begin(), gone.end(), [](const ExpiredTask& a, const ExpiredTask& b) { return a.id < b.id; });
    std::vector<ExpiredTask> expected{{running.id, TaskStatus::working}, {finished.id, TaskStatus::completed}};
    std::sort(expected.begin(), expected.end(), [](const ExpiredTask& a, const ExpiredTask& b) { return a.id < b.id; });
    ASSERT_EQ(gone.size(), 2u);
    for (std::size_t i = 0; i < gone.size(); ++i) {
        EXPECT_EQ(gone[i].id, expected[i].id);
        EXPECT_EQ(gone[i].status, expected[i].status);
    }
    EXPECT_EQ(store->usage().tasks, 1u);
    EXPECT_TRUE(store->get(later.id, TaskOwner{}).has_value());
    EXPECT_TRUE(store->expire(ticks.now()).empty()) << "a second sweep finds nothing";
}

TEST(TaskStoreExpiry, AnExpiredTaskCannotBeChangedAnsweredOrWatched) {
    FakeClock ticks;
    auto store = make_memory_task_store(options_with(ticks));
    const TaskRecord record = make_task(*store, draft("", Millis(1000)));
    ticks.advance(Millis(1000));
    EXPECT_EQ(step(*store, record.id, TaskStatus::working, to(TaskStatus::completed)).outcome,
              TransitionOutcome::not_found);
    EXPECT_EQ(store->put_answers(record.id, TaskOwner{}, json::object()).outcome, AnswersOutcome::not_found);
    EXPECT_FALSE(store->take_answers(record.id).has_value());
    auto recorder = std::make_shared<Recorder>();
    EXPECT_EQ(store->watch(record.id, TaskOwner{}, recorder).outcome, WatchOutcome::not_found);
    EXPECT_EQ(store->usage().tasks, 0u) << "the change that found it expired removed it";
}

TEST(TaskStoreExpiry, ALifetimeLongerThanTheClockCanCountDoesNotWrapAround) {
    FakeClock ticks;
    MemoryTaskStoreOptions options = options_with(ticks);
    options.max_ttl = std::chrono::duration_cast<Millis>(std::chrono::hours(24) * 365 * 1000);  // a thousand years
    auto store = make_memory_task_store(options);
    const TaskRecord record = make_task(*store, draft("", options.max_ttl));
    EXPECT_EQ(record.ttl, std::optional<Millis>(options.max_ttl));
    ticks.advance(std::chrono::duration_cast<Millis>(std::chrono::hours(24)));
    EXPECT_TRUE(store->get(record.id, TaskOwner{}).has_value()) << "the end of the lifetime is out of reach, not in the past";
    EXPECT_TRUE(store->expire(ticks.now()).empty());
}

TEST(TaskStoreExpiry, ACreateSweepsWhatEndedBeforeItChecksTheBounds) {
    FakeClock ticks;
    MemoryTaskStoreOptions options = options_with(ticks);
    options.max_tasks = 2;
    auto store = make_memory_task_store(options);
    make_task(*store, draft("", Millis(1000)));
    make_task(*store, draft("", Millis(1000)));
    EXPECT_EQ(refusal_of(*store), CreateRefusal::store_full);
    ticks.advance(Millis(1000));
    make_task(*store);  // the two that ended make room
    EXPECT_EQ(store->usage().tasks, 1u);
}

// ---------------------------------------------------------------------------------------------------------------
// Owners

TEST(TaskStoreOwners, AnotherOwnerSeesNoTask) {
    FakeClock ticks;
    auto store = make_memory_task_store(options_with(ticks));
    const TaskRecord record = make_task(*store, draft("alice"));
    EXPECT_TRUE(store->get(record.id, TaskOwner{"alice"}).has_value());
    EXPECT_FALSE(store->get(record.id, TaskOwner{"bob"}).has_value());
    EXPECT_FALSE(store->get(record.id, TaskOwner{}).has_value()) << "an empty owner is not every owner";
    EXPECT_EQ(store->put_answers(record.id, TaskOwner{"bob"}, json::object()).outcome, AnswersOutcome::not_found);
    auto recorder = std::make_shared<Recorder>();
    EXPECT_EQ(store->watch(record.id, TaskOwner{"bob"}, recorder).outcome, WatchOutcome::not_found);

    const TaskRecord open = make_task(*store, draft(""));
    EXPECT_TRUE(store->get(open.id, TaskOwner{}).has_value());
    EXPECT_FALSE(store->get(open.id, TaskOwner{"alice"}).has_value()) << "a named owner is not the empty owner";
}

// ---------------------------------------------------------------------------------------------------------------
// Transitions

TEST(TaskStoreTransition, EveryPairOfStatuses) {
    for (const TaskStatus start : all_statuses) {
        for (const TaskStatus target : all_statuses) {
            SCOPED_TRACE(std::string(to_string(start)) + " -> " + std::string(to_string(target)));
            FakeClock ticks;
            auto store = make_memory_task_store(options_with(ticks));
            const TaskRecord record = make_task(*store);
            if (start != TaskStatus::working) {
                ASSERT_EQ(step(*store, record.id, TaskStatus::working, to(start)).outcome, TransitionOutcome::applied);
            }
            ticks.advance(Millis(10));
            const Transition result = step(*store, record.id, start, to(target));
            const auto after = store->get(record.id, TaskOwner{});
            ASSERT_TRUE(after.has_value());
            if (is_terminal(start)) {
                EXPECT_EQ(result.outcome, TransitionOutcome::status_mismatch) << "an ended task never changes";
                ASSERT_TRUE(result.record.has_value());
                EXPECT_EQ(result.record->status, start);
                EXPECT_EQ(after->status, start);
            } else {
                EXPECT_EQ(result.outcome, TransitionOutcome::applied);
                ASSERT_TRUE(result.record.has_value());
                EXPECT_EQ(result.record->status, target);
                EXPECT_EQ(after->status, target);
                EXPECT_EQ(after->updated, ticks.now());
                EXPECT_EQ(after->created, record.created);
            }
        }
    }
}

TEST(TaskStoreTransition, AFailedOrCancelledTaskIsNotRestarted) {
    FakeClock ticks;
    auto store = make_memory_task_store(options_with(ticks));
    for (const TaskStatus ended : {TaskStatus::failed, TaskStatus::cancelled, TaskStatus::completed}) {
        SCOPED_TRACE(std::string(to_string(ended)));
        const TaskRecord record = make_task(*store);
        ASSERT_EQ(step(*store, record.id, TaskStatus::working, to(ended)).outcome, TransitionOutcome::applied);
        // Even a caller that lists the ended status among those it expects cannot take the task out of it.
        const TaskStatus expected[] = {ended, TaskStatus::working};
        for (const TaskStatus again : {TaskStatus::working, TaskStatus::input_required, TaskStatus::completed}) {
            const Transition result = store->transition(record.id, expected, to(again));
            EXPECT_EQ(result.outcome, TransitionOutcome::status_mismatch);
            EXPECT_EQ(store->get(record.id, TaskOwner{})->status, ended);
        }
    }
}

TEST(TaskStoreTransition, OnlyAnExpectedStatusIsLeft) {
    FakeClock ticks;
    auto store = make_memory_task_store(options_with(ticks));
    const TaskRecord record = make_task(*store);
    const Transition wrong = step(*store, record.id, TaskStatus::input_required, to(TaskStatus::completed));
    EXPECT_EQ(wrong.outcome, TransitionOutcome::status_mismatch);
    ASSERT_TRUE(wrong.record.has_value());
    EXPECT_EQ(wrong.record->status, TaskStatus::working) << "the answer tells what was found";
    const Transition none = store->transition(record.id, {}, to(TaskStatus::completed));
    EXPECT_EQ(none.outcome, TransitionOutcome::status_mismatch);
    EXPECT_EQ(step(*store, "no-such-task", TaskStatus::working, to(TaskStatus::completed)).outcome,
              TransitionOutcome::not_found);
}

TEST(TaskStoreTransition, TheOutcomeAndTheMessageAreKept) {
    FakeClock ticks;
    auto store = make_memory_task_store(options_with(ticks));
    const TaskRecord record = make_task(*store);

    TaskUpdate progress = to(TaskStatus::working);
    progress.message = "halfway";
    ASSERT_EQ(step(*store, record.id, TaskStatus::working, progress).outcome, TransitionOutcome::applied);
    EXPECT_EQ(store->get(record.id, TaskOwner{})->status_message, std::optional<std::string>("halfway"));

    TaskUpdate done = to(TaskStatus::completed);
    done.result = json{{"content", json::array()}};
    ASSERT_EQ(step(*store, record.id, TaskStatus::working, done).outcome, TransitionOutcome::applied);
    const auto after = store->get(record.id, TaskOwner{});
    ASSERT_TRUE(after.has_value());
    EXPECT_FALSE(after->status_message.has_value()) << "a message not given again is cleared";
    ASSERT_TRUE(after->result.has_value());
    EXPECT_EQ(*after->result, *done.result);
    EXPECT_FALSE(after->error.has_value());

    const TaskRecord failing = make_task(*store);
    TaskUpdate failed = to(TaskStatus::failed);
    failed.error = json{{"code", -32603}, {"message", "boom"}};
    failed.message = "boom";
    ASSERT_EQ(step(*store, failing.id, TaskStatus::working, failed).outcome, TransitionOutcome::applied);
    const auto lost = store->get(failing.id, TaskOwner{});
    ASSERT_TRUE(lost.has_value());
    ASSERT_TRUE(lost->error.has_value());
    EXPECT_EQ(*lost->error, *failed.error);
    EXPECT_EQ(lost->status_message, std::optional<std::string>("boom"));
}

// ---------------------------------------------------------------------------------------------------------------
// Input

TEST(TaskStoreInput, AnswersAreKeptForAskedKeysAndTakenOnceWhenComplete) {
    FakeClock ticks;
    auto store = make_memory_task_store(options_with(ticks));
    const TaskRecord record = make_task(*store, draft("alice"));
    const TaskOwner alice{"alice"};

    // Not waiting yet.
    EXPECT_EQ(store->put_answers(record.id, alice, json{{"a", 1}}).outcome, AnswersOutcome::not_waiting);
    EXPECT_FALSE(store->take_answers(record.id).has_value());

    TaskUpdate ask = to(TaskStatus::input_required);
    ask.asked = json{{"a", json{{"method", "elicitation/create"}}}, {"b", json{{"method", "roots/list"}}}};
    ASSERT_EQ(step(*store, record.id, TaskStatus::working, ask).outcome, TransitionOutcome::applied);
    EXPECT_FALSE(store->take_answers(record.id).has_value()) << "nothing is answered yet";

    // An answer to a key that was not asked is dropped; a first answer to an asked key is kept.
    AnswersResult first = store->put_answers(record.id, alice, json{{"a", json{{"action", "accept"}}}, {"zzz", 1}});
    ASSERT_EQ(first.outcome, AnswersOutcome::applied);
    ASSERT_TRUE(first.record->answers.has_value());
    EXPECT_TRUE(first.record->answers->contains("a"));
    EXPECT_FALSE(first.record->answers->contains("zzz"));
    EXPECT_FALSE(store->take_answers(record.id).has_value()) << "b is still missing";

    // A second answer to the same key does not replace the first.
    AnswersResult again = store->put_answers(record.id, alice, json{{"a", json{{"action", "decline"}}}});
    ASSERT_EQ(again.outcome, AnswersOutcome::applied);
    EXPECT_EQ((*again.record->answers)["a"]["action"], "accept");

    ASSERT_EQ(store->put_answers(record.id, alice, json{{"b", json{{"roots", json::array()}}}}).outcome,
              AnswersOutcome::applied);
    const std::optional<json> taken = store->take_answers(record.id);
    ASSERT_TRUE(taken.has_value());
    EXPECT_TRUE(taken->contains("a"));
    EXPECT_TRUE(taken->contains("b"));
    EXPECT_FALSE(store->take_answers(record.id).has_value()) << "the answers are taken once";

    const auto after = store->get(record.id, alice);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->status, TaskStatus::working);
    EXPECT_FALSE(after->asked.has_value());
    EXPECT_FALSE(after->answers.has_value());
}

TEST(TaskStoreInput, AskingAgainStartsANewRoundAndLeavingInputClearsIt) {
    FakeClock ticks;
    auto store = make_memory_task_store(options_with(ticks));
    const TaskRecord record = make_task(*store);
    TaskUpdate ask = to(TaskStatus::input_required);
    ask.asked = json{{"a", 1}};
    ASSERT_EQ(step(*store, record.id, TaskStatus::working, ask).outcome, TransitionOutcome::applied);
    ASSERT_EQ(store->put_answers(record.id, TaskOwner{}, json{{"a", 5}}).outcome, AnswersOutcome::applied);

    TaskUpdate next = to(TaskStatus::input_required);
    next.asked = json{{"b", 2}};
    ASSERT_EQ(step(*store, record.id, TaskStatus::input_required, next).outcome, TransitionOutcome::applied);
    auto state = store->get(record.id, TaskOwner{});
    ASSERT_TRUE(state->asked.has_value());
    EXPECT_TRUE(state->asked->contains("b"));
    EXPECT_FALSE(state->asked->contains("a"));
    EXPECT_FALSE(state->answers.has_value()) << "a new round starts without the old answers";

    ASSERT_EQ(step(*store, record.id, TaskStatus::input_required, to(TaskStatus::cancelled)).outcome,
              TransitionOutcome::applied);
    state = store->get(record.id, TaskOwner{});
    EXPECT_FALSE(state->asked.has_value());
}

// ---------------------------------------------------------------------------------------------------------------
// Bounds

TEST(TaskStoreBounds, TheNumberOfTasksAtAndPastTheBound) {
    FakeClock ticks;
    MemoryTaskStoreOptions options = options_with(ticks);
    options.max_tasks = 5;
    options.max_tasks_per_owner = 5;
    auto store = make_memory_task_store(options);
    for (int i = 0; i < 5; ++i) {
        make_task(*store, draft("owner" + std::to_string(i)));
    }
    EXPECT_EQ(store->usage().tasks, 5u);
    EXPECT_EQ(refusal_of(*store, draft("another")), CreateRefusal::store_full);
    EXPECT_EQ(store->usage().tasks, 5u);
}

TEST(TaskStoreBounds, TheNumberOfTasksOfOneOwnerAtAndPastTheBound) {
    FakeClock ticks;
    MemoryTaskStoreOptions options = options_with(ticks);
    options.max_tasks = 100;
    options.max_tasks_per_owner = 3;
    auto store = make_memory_task_store(options);
    for (int i = 0; i < 3; ++i) {
        make_task(*store, draft("alice"));
        make_task(*store, draft(""));
    }
    EXPECT_EQ(refusal_of(*store, draft("alice")), CreateRefusal::owner_full);
    EXPECT_EQ(refusal_of(*store, draft("")), CreateRefusal::owner_full) << "the empty owner is one owner";
    make_task(*store, draft("bob"));  // another owner is not held back
    EXPECT_EQ(store->usage().tasks, 7u);
}

TEST(TaskStoreBounds, TheBytesAllRecordsTakeAtAndPastTheBudget) {
    FakeClock ticks;
    MemoryTaskStoreOptions options = options_with(ticks);
    {
        auto probe = make_memory_task_store(options);
        make_task(*probe, draft("owner"));
        const std::size_t one = probe->usage().bytes;
        ASSERT_GT(one, 0u);
        // The budget holds exactly three records of this size.
        options.max_stored_bytes = 3 * one;
        options.max_tasks = 100;
        options.max_tasks_per_owner = 100;
    }
    auto store = make_memory_task_store(options);
    for (int i = 0; i < 3; ++i) {
        make_task(*store, draft("owner"));
    }
    EXPECT_EQ(store->usage().bytes, options.max_stored_bytes);
    EXPECT_EQ(refusal_of(*store, draft("owner")), CreateRefusal::byte_budget);
    EXPECT_EQ(store->usage().tasks, 3u);
    EXPECT_EQ(store->usage().bytes, options.max_stored_bytes);
}

TEST(TaskStoreBounds, ARequestLargerThanTheBudgetIsRefused) {
    FakeClock ticks;
    MemoryTaskStoreOptions options = options_with(ticks);
    options.max_stored_bytes = 4096;
    auto store = make_memory_task_store(options);
    TaskDraft large = draft();
    large.request = text_of_size(5000);
    EXPECT_EQ(refusal_of(*store, std::move(large)), CreateRefusal::byte_budget);
    EXPECT_EQ(store->usage().tasks, 0u);
    EXPECT_EQ(store->usage().bytes, 0u);
}

TEST(TaskStoreBounds, AResultAtAndPastTheLargestSize) {
    FakeClock ticks;
    MemoryTaskStoreOptions options = options_with(ticks);
    options.max_result_bytes = 1000;
    auto store = make_memory_task_store(options);
    const TaskRecord at = make_task(*store);
    TaskUpdate fits = to(TaskStatus::completed);
    fits.result = text_of_size(1000);
    EXPECT_EQ(step(*store, at.id, TaskStatus::working, fits).outcome, TransitionOutcome::applied);

    const TaskRecord past = make_task(*store);
    TaskUpdate large = to(TaskStatus::completed);
    large.result = text_of_size(1001);
    const Transition refused = step(*store, past.id, TaskStatus::working, large);
    EXPECT_EQ(refused.outcome, TransitionOutcome::too_large);
    EXPECT_EQ(store->get(past.id, TaskOwner{})->status, TaskStatus::working) << "nothing changed";
    EXPECT_FALSE(store->get(past.id, TaskOwner{})->result.has_value());

    // An error and a request for input are held to the same size.
    TaskUpdate large_error = to(TaskStatus::failed);
    large_error.error = text_of_size(1001);
    EXPECT_EQ(step(*store, past.id, TaskStatus::working, large_error).outcome, TransitionOutcome::too_large);
    TaskUpdate large_ask = to(TaskStatus::input_required);
    large_ask.asked = json{{"key", std::string(1001, 'x')}};
    EXPECT_EQ(step(*store, past.id, TaskStatus::working, large_ask).outcome, TransitionOutcome::too_large);
}

TEST(TaskStoreBounds, AResultThatWouldPassTheBudgetIsRefusedAndASmallFailureAlwaysFits) {
    FakeClock ticks;
    MemoryTaskStoreOptions options = options_with(ticks);
    options.max_result_bytes = 100'000;
    options.outcome_reserve_bytes = 512;
    std::size_t one = 0;
    {
        auto probe = make_memory_task_store(options);
        make_task(*probe);
        one = probe->usage().bytes;
    }
    options.max_stored_bytes = 2 * one + 3000;
    auto store = make_memory_task_store(options);
    const TaskRecord first = make_task(*store);
    const TaskRecord second = make_task(*store);
    ASSERT_EQ(store->usage().bytes, 2 * one);

    // A result of 3001 bytes needs more room than the budget has left.
    TaskUpdate large = to(TaskStatus::completed);
    large.result = text_of_size(3001 + options.outcome_reserve_bytes);
    EXPECT_EQ(step(*store, first.id, TaskStatus::working, large).outcome, TransitionOutcome::over_budget);
    EXPECT_EQ(store->usage().bytes, 2 * one) << "nothing changed";
    EXPECT_EQ(store->get(first.id, TaskOwner{})->status, TaskStatus::working);

    // The task can still end: an error that says why fits in what the task reserved.
    TaskUpdate failure = to(TaskStatus::failed);
    failure.error = json{{"code", -32603}, {"message", "the result was too large"}};
    failure.message = "the result was too large";
    EXPECT_EQ(step(*store, first.id, TaskStatus::working, failure).outcome, TransitionOutcome::applied);
    EXPECT_LE(store->usage().bytes, options.max_stored_bytes);

    // A result of the size that does fit is taken.
    TaskUpdate fits = to(TaskStatus::completed);
    fits.result = text_of_size(2000);
    EXPECT_EQ(step(*store, second.id, TaskStatus::working, fits).outcome, TransitionOutcome::applied);
    EXPECT_LE(store->usage().bytes, options.max_stored_bytes);
}

TEST(TaskStoreBounds, AnAnswerThatWouldPassTheBudgetOrTheSizeIsRefused) {
    FakeClock ticks;
    MemoryTaskStoreOptions options = options_with(ticks);
    options.max_result_bytes = 1000;
    auto store = make_memory_task_store(options);
    const TaskRecord record = make_task(*store);
    TaskUpdate ask = to(TaskStatus::input_required);
    ask.asked = json{{"a", 1}};
    ASSERT_EQ(step(*store, record.id, TaskStatus::working, ask).outcome, TransitionOutcome::applied);
    const AnswersResult large = store->put_answers(record.id, TaskOwner{}, json{{"a", std::string(1001, 'x')}});
    EXPECT_EQ(large.outcome, AnswersOutcome::too_large);
    EXPECT_FALSE(store->get(record.id, TaskOwner{})->answers.has_value());
}

// ---------------------------------------------------------------------------------------------------------------
// Listing

TEST(TaskStoreList, PagesAreOldestFirstAndStableWhileTasksExpire) {
    FakeClock ticks;
    auto store = make_memory_task_store(options_with(ticks));
    std::vector<std::string> ids;
    for (int i = 0; i < 7; ++i) {
        ticks.advance(Millis(1));
        // The third and the fifth are short-lived.
        const std::optional<Millis> ttl = (i == 2 || i == 4) ? std::optional<Millis>(Millis(50)) : std::nullopt;
        ids.push_back(make_task(*store, draft("alice", ttl)).id);
    }
    make_task(*store, draft("bob"));

    const TaskOwner alice{"alice"};
    auto page = store->list(alice, std::nullopt, 3);
    ASSERT_TRUE(page.has_value());
    ASSERT_EQ(page->tasks.size(), 3u);
    EXPECT_EQ(page->tasks[0].id, ids[0]);
    EXPECT_EQ(page->tasks[1].id, ids[1]);
    EXPECT_EQ(page->tasks[2].id, ids[2]);
    ASSERT_TRUE(page->next_cursor.has_value());
    EXPECT_TRUE(page->tasks[0].request.is_null()) << "a listing leaves out the request";
    EXPECT_FALSE(page->tasks[0].result.has_value());

    // Tasks of the first and the next page expire between the calls: the next page neither repeats nor skips.
    ticks.advance(Millis(100));
    store->expire(ticks.now());
    auto second = store->list(alice, *page->next_cursor, 3);
    ASSERT_TRUE(second.has_value());
    std::vector<std::string> seen;
    for (const auto& task : second->tasks) seen.push_back(task.id);
    EXPECT_EQ(seen, (std::vector<std::string>{ids[3], ids[5], ids[6]})) << "ids[4] expired, the rest follow in order";
    EXPECT_FALSE(second->next_cursor.has_value());
}

TEST(TaskStoreList, OnlyTheOwnersTasksAndAnInvalidCursor) {
    FakeClock ticks;
    MemoryTaskStoreOptions options = options_with(ticks);
    options.max_page_size = 4;
    auto store = make_memory_task_store(options);
    for (int i = 0; i < 6; ++i) {
        make_task(*store, draft("alice"));
        make_task(*store, draft("bob"));
    }
    const auto first = store->list(TaskOwner{"alice"}, std::nullopt, 100);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->tasks.size(), 4u) << "a page is at most max_page_size";
    for (const auto& task : first->tasks) EXPECT_EQ(task.owner.principal, "alice");
    ASSERT_TRUE(first->next_cursor.has_value());
    const auto rest = store->list(TaskOwner{"alice"}, *first->next_cursor, 0);
    ASSERT_TRUE(rest.has_value());
    EXPECT_EQ(rest->tasks.size(), 2u);
    EXPECT_FALSE(rest->next_cursor.has_value());
    EXPECT_TRUE(store->list(TaskOwner{"nobody"}, std::nullopt, 10)->tasks.empty());

    EXPECT_FALSE(store->list(TaskOwner{"alice"}, "not a cursor", 10).has_value());
    EXPECT_FALSE(store->list(TaskOwner{"alice"}, "", 10).has_value());
    EXPECT_FALSE(store->list(TaskOwner{"alice"}, "12x", 10).has_value());
    EXPECT_FALSE(store->list(TaskOwner{"alice"}, "-1", 10).has_value());
    EXPECT_FALSE(store->list(TaskOwner{"alice"}, "99999999999999999999999999", 10).has_value());
}

// ---------------------------------------------------------------------------------------------------------------
// Watching

TEST(TaskStoreWatch, ReportsTheStateAtTheStartAndEveryLaterChange) {
    FakeClock ticks;
    auto store = make_memory_task_store(options_with(ticks));
    const TaskRecord record = make_task(*store, draft("alice", Millis(1000)));
    auto recorder = std::make_shared<Recorder>();
    TaskWatchStart start = store->watch(record.id, TaskOwner{"alice"}, recorder);
    ASSERT_EQ(start.outcome, WatchOutcome::watching);
    EXPECT_TRUE(static_cast<bool>(start.watch));
    ASSERT_TRUE(start.record.has_value());
    EXPECT_EQ(start.record->status, TaskStatus::working);
    EXPECT_TRUE(recorder->changes().empty()) << "nothing has changed since the start";

    ASSERT_EQ(step(*store, record.id, TaskStatus::working, to(TaskStatus::input_required)).outcome,
              TransitionOutcome::applied);
    ASSERT_EQ(step(*store, record.id, TaskStatus::input_required, to(TaskStatus::completed)).outcome,
              TransitionOutcome::applied);
    auto changes = recorder->changes();
    ASSERT_EQ(changes.size(), 2u);
    EXPECT_EQ(changes[0].kind, TaskChangeKind::updated);
    EXPECT_EQ(changes[0].status, TaskStatus::input_required);
    EXPECT_EQ(changes[0].id, record.id);
    EXPECT_EQ(changes[1].status, TaskStatus::completed);

    ticks.advance(Millis(1000));
    store->expire(ticks.now());
    changes = recorder->changes();
    ASSERT_EQ(changes.size(), 3u);
    EXPECT_EQ(changes[2].kind, TaskChangeKind::removed);
    EXPECT_EQ(changes[2].status, TaskStatus::completed);
}

TEST(TaskStoreWatch, AReleasedOrDestroyedListenerHearsNothingMore) {
    FakeClock ticks;
    auto store = make_memory_task_store(options_with(ticks));
    const TaskRecord record = make_task(*store);
    auto kept = std::make_shared<Recorder>();
    auto released = std::make_shared<Recorder>();
    auto destroyed = std::make_shared<Recorder>();
    TaskWatchStart a = store->watch(record.id, TaskOwner{}, kept);
    TaskWatchStart b = store->watch(record.id, TaskOwner{}, released);
    TaskWatchStart c = store->watch(record.id, TaskOwner{}, destroyed);
    b.watch.release();
    EXPECT_FALSE(static_cast<bool>(b.watch));
    destroyed.reset();  // the registration is still held, the listener is gone: the store must not call it
    ASSERT_EQ(step(*store, record.id, TaskStatus::working, to(TaskStatus::completed)).outcome,
              TransitionOutcome::applied);
    EXPECT_EQ(kept->changes().size(), 1u);
    EXPECT_TRUE(released->changes().empty());
}

TEST(TaskStoreWatch, ARegistrationOutlivesTheStore) {
    FakeClock ticks;
    TaskWatch survivor;
    {
        auto store = make_memory_task_store(options_with(ticks));
        const TaskRecord record = make_task(*store);
        auto recorder = std::make_shared<Recorder>();
        TaskWatchStart start = store->watch(record.id, TaskOwner{}, recorder);
        ASSERT_EQ(start.outcome, WatchOutcome::watching);
        survivor = std::move(start.watch);
    }
    survivor.release();  // the store is gone: nothing to do, and nothing to fault
}

TEST(TaskStoreWatch, AListenerMayCallTheStore) {
    FakeClock ticks;
    auto store = make_memory_task_store(options_with(ticks));
    const TaskRecord record = make_task(*store);
    struct Reader final : TaskListener {
        TaskStore* store = nullptr;
        TaskRecord* seen = nullptr;
        std::string id;
        void on_task_change(const TaskChange&) override {
            const auto found = store->get(id, TaskOwner{});
            if (found) *seen = *found;
        }
    };
    TaskRecord seen;
    auto reader = std::make_shared<Reader>();
    reader->store = store.get();
    reader->seen = &seen;
    reader->id = record.id;
    TaskWatchStart start = store->watch(record.id, TaskOwner{}, reader);
    ASSERT_EQ(start.outcome, WatchOutcome::watching);
    ASSERT_EQ(step(*store, record.id, TaskStatus::working, to(TaskStatus::completed)).outcome,
              TransitionOutcome::applied);
    EXPECT_EQ(seen.status, TaskStatus::completed) << "the change is visible when the listener is called";
}

TEST(TaskStoreWatch, TheNumberOfWatchesIsBounded) {
    FakeClock ticks;
    MemoryTaskStoreOptions options = options_with(ticks);
    options.max_watches = 2;
    auto store = make_memory_task_store(options);
    const TaskRecord record = make_task(*store);
    auto recorder = std::make_shared<Recorder>();
    TaskWatchStart one = store->watch(record.id, TaskOwner{}, recorder);
    TaskWatchStart two = store->watch(record.id, TaskOwner{}, recorder);
    EXPECT_EQ(one.outcome, WatchOutcome::watching);
    EXPECT_EQ(two.outcome, WatchOutcome::watching);
    EXPECT_EQ(store->watch(record.id, TaskOwner{}, recorder).outcome, WatchOutcome::limit);
    one.watch.release();
    EXPECT_EQ(store->watch(record.id, TaskOwner{}, recorder).outcome, WatchOutcome::watching) << "room again";
}

// ---------------------------------------------------------------------------------------------------------------
// Recovery

TEST(TaskStoreRecover, EveryUnfinishedTaskFails) {
    FakeClock ticks;
    auto store = make_memory_task_store(options_with(ticks));
    const TaskRecord working = make_task(*store);
    const TaskRecord waiting = make_task(*store);
    const TaskRecord finished = make_task(*store);
    ASSERT_EQ(step(*store, waiting.id, TaskStatus::working, to(TaskStatus::input_required)).outcome,
              TransitionOutcome::applied);
    TaskUpdate done = to(TaskStatus::completed);
    done.result = json{{"ok", true}};
    ASSERT_EQ(step(*store, finished.id, TaskStatus::working, done).outcome, TransitionOutcome::applied);

    TaskUpdate failure = to(TaskStatus::failed);
    failure.message = "the server restarted";
    failure.error = json{{"code", -32603}, {"message", "the server restarted"}};
    EXPECT_EQ(store->recover(failure), 2u);
    for (const TaskRecord* record : {&working, &waiting}) {
        const auto after = store->get(record->id, TaskOwner{});
        ASSERT_TRUE(after.has_value());
        EXPECT_EQ(after->status, TaskStatus::failed);
        EXPECT_EQ(after->status_message, std::optional<std::string>("the server restarted"));
        ASSERT_TRUE(after->error.has_value());
    }
    EXPECT_EQ(store->get(finished.id, TaskOwner{})->status, TaskStatus::completed);
    EXPECT_EQ(store->recover(failure), 0u);
}

TEST(TaskStoreRecover, RefusesAnUpdateThatIsNotATerminalSmallFailure) {
    FakeClock ticks;
    MemoryTaskStoreOptions options = options_with(ticks);
    options.outcome_reserve_bytes = 128;
    auto store = make_memory_task_store(options);
    make_task(*store);
    EXPECT_THROW(store->recover(to(TaskStatus::working)), std::invalid_argument);
    TaskUpdate large = to(TaskStatus::failed);
    large.error = text_of_size(500);
    EXPECT_THROW(store->recover(large), std::invalid_argument);
    EXPECT_EQ(store->usage().tasks, 1u);
}

// ---------------------------------------------------------------------------------------------------------------
// Options

TEST(TaskStoreOptions, AnOptionThatIsZeroOrInconsistentIsRefused) {
    const auto refused = [](auto change) {
        MemoryTaskStoreOptions options;
        change(options);
        EXPECT_THROW(make_memory_task_store(options), std::invalid_argument);
    };
    refused([](MemoryTaskStoreOptions& o) { o.max_tasks = 0; });
    refused([](MemoryTaskStoreOptions& o) { o.max_tasks_per_owner = 0; });
    refused([](MemoryTaskStoreOptions& o) { o.default_ttl = Millis(0); });
    refused([](MemoryTaskStoreOptions& o) { o.min_ttl = Millis(0); });
    refused([](MemoryTaskStoreOptions& o) { o.max_ttl = Millis(0); });
    refused([](MemoryTaskStoreOptions& o) { o.poll_interval = Millis(0); });
    refused([](MemoryTaskStoreOptions& o) { o.max_result_bytes = 0; });
    refused([](MemoryTaskStoreOptions& o) { o.max_stored_bytes = 0; });
    refused([](MemoryTaskStoreOptions& o) { o.max_page_size = 0; });
    refused([](MemoryTaskStoreOptions& o) { o.max_watches = 0; });
    refused([](MemoryTaskStoreOptions& o) { o.id_attempts = 0; });
    refused([](MemoryTaskStoreOptions& o) {
        o.min_ttl = Millis(20);
        o.max_ttl = Millis(10);
        o.default_ttl = Millis(15);
    });
    refused([](MemoryTaskStoreOptions& o) {
        o.min_ttl = Millis(10);
        o.max_ttl = Millis(100);
        o.default_ttl = Millis(5);
    });
    refused([](MemoryTaskStoreOptions& o) {
        o.min_ttl = Millis(10);
        o.max_ttl = Millis(100);
        o.default_ttl = Millis(500);
    });
    MemoryTaskStoreOptions fine;
    fine.outcome_reserve_bytes = 0;
    EXPECT_NO_THROW(make_memory_task_store(fine));
    EXPECT_NO_THROW(make_memory_task_store());
}

// ---------------------------------------------------------------------------------------------------------------
// Races (the store is used from several threads)

// Inside a loop whose threads must be stopped and joined: report a failure and leave the loop, instead of returning
// (which would destroy joinable threads).
#define RACE_CHECK(condition, message)                              \
    if (!(condition)) {                                             \
        ADD_FAILURE() << (message) << " (iteration " << iteration << ")";   \
        break;                                                      \
    }                                                               \
    static_cast<void>(0)

// Two callers race to end one task, in both orders: one wins, the other is told what it found, and the task never
// changes again. In half of the rounds the order is forced (the second caller waits until the first has finished), in
// the other half the callers race freely.
TEST(TaskStoreRaces, CancelAgainstCompletionLeavesOneTerminalStatus) {
    constexpr int rounds = 10'000;
    FakeClock ticks;
    MemoryTaskStoreOptions options = options_with(ticks);
    options.default_ttl = Millis(1000);
    options.max_tasks = 8;
    auto store = make_memory_task_store(options);

    std::string id;
    Rendezvous start(3);
    Rendezvous finish(3);
    std::atomic<bool> stop{false};
    std::atomic<int> turn_now{0};
    struct Side {
        TaskStatus ending;
        bool cancels;
        std::atomic<int> outcome{-1};
        std::atomic<int> found{-1};
        std::atomic<int> done{-1};  // the last iteration this side finished
    };
    Side canceller{TaskStatus::cancelled, true};
    Side completer{TaskStatus::completed, false};
    const auto run = [&](Side& self, Side& other) {
        for (;;) {
            start.arrive_and_wait();
            if (stop.load()) return;
            const int turn = turn_now.load();
            const int mode = turn % 4;  // 0: the canceller first, 1: the completer first, 2 and 3: free
            const bool second = (mode == 0 && !self.cancels) || (mode == 1 && self.cancels);
            if (second) {
                while (other.done.load() != turn) std::this_thread::yield();
            }
            const Transition result = step(*store, id, TaskStatus::working, to(self.ending));
            self.outcome = static_cast<int>(result.outcome);
            self.found = result.record ? static_cast<int>(result.record->status) : -1;
            self.done = turn;
            finish.arrive_and_wait();
        }
    };
    std::thread cancelling(run, std::ref(canceller), std::ref(completer));
    std::thread completing(run, std::ref(completer), std::ref(canceller));

    int cancelled_wins = 0;
    int completed_wins = 0;
    for (int iteration = 0; iteration < rounds; ++iteration) {
        ticks.advance(Millis(2000));
        store->expire(ticks.now());
        id = make_task(*store).id;
        turn_now = iteration;
        start.arrive_and_wait();
        finish.arrive_and_wait();
        const bool cancel_applied = canceller.outcome == static_cast<int>(TransitionOutcome::applied);
        const bool complete_applied = completer.outcome == static_cast<int>(TransitionOutcome::applied);
        RACE_CHECK(cancel_applied != complete_applied, "exactly one transition wins");
        const TaskStatus winner = cancel_applied ? TaskStatus::cancelled : TaskStatus::completed;
        (cancel_applied ? cancelled_wins : completed_wins) += 1;
        Side& loser = cancel_applied ? completer : canceller;
        RACE_CHECK(loser.outcome.load() == static_cast<int>(TransitionOutcome::status_mismatch), "the loser is refused");
        RACE_CHECK(loser.found.load() == static_cast<int>(winner), "the loser is told the winner's status");
        RACE_CHECK(store->get(id, TaskOwner{})->status == winner, "the task never changes after");
        if (iteration % 4 == 0) {
            RACE_CHECK(cancel_applied, "the caller that went first won");
        } else if (iteration % 4 == 1) {
            RACE_CHECK(complete_applied, "the caller that went first won");
        }
    }
    stop = true;
    start.arrive_and_wait();
    cancelling.join();
    completing.join();
    EXPECT_GE(cancelled_wins, rounds / 4) << "both orders were exercised";
    EXPECT_GE(completed_wins, rounds / 4) << "both orders were exercised";
}

// A task expires while it is read, watched and swept from other threads: a waiter is told, never left waiting, and
// nothing is used after it is removed.
TEST(TaskStoreRaces, ExpiryAgainstAPollAndAWaiterLeavesNobodyWaiting) {
    constexpr int rounds = 2000;
    FakeClock ticks;
    MemoryTaskStoreOptions options = options_with(ticks);
    options.min_ttl = Millis(1);
    options.max_tasks = 8;
    auto store = make_memory_task_store(options);

    std::string id;
    std::shared_ptr<Recorder> late;
    TaskWatchStart late_start;
    Rendezvous start(4);
    Rendezvous finish(4);
    std::atomic<bool> stop{false};
    std::atomic<bool> poll_found{false};

    std::thread sweeper([&] {
        for (;;) {
            start.arrive_and_wait();
            if (stop.load()) return;
            ticks.advance(Millis(100));
            store->expire(ticks.now());
            finish.arrive_and_wait();
        }
    });
    std::thread poller([&] {
        for (;;) {
            start.arrive_and_wait();
            if (stop.load()) return;
            poll_found = store->get(id, TaskOwner{}).has_value();
            finish.arrive_and_wait();
        }
    });
    std::thread watcher([&] {
        for (;;) {
            start.arrive_and_wait();
            if (stop.load()) return;
            late = std::make_shared<Recorder>();
            late_start = store->watch(id, TaskOwner{}, late);
            finish.arrive_and_wait();
        }
    });

    for (int iteration = 0; iteration < rounds; ++iteration) {
        id = make_task(*store, draft("", Millis(10))).id;
        auto early = std::make_shared<Recorder>();
        TaskWatchStart before = store->watch(id, TaskOwner{}, early);
        RACE_CHECK(before.outcome == WatchOutcome::watching, "the watch of a new task starts");
        start.arrive_and_wait();
        finish.arrive_and_wait();

        RACE_CHECK(early->count(TaskChangeKind::removed) == 1u, "a waiter registered before is told, once");
        if (late_start.outcome == WatchOutcome::watching) {
            RACE_CHECK(late->count(TaskChangeKind::removed) == 1u, "a waiter that registered in time is told, once");
        } else {
            RACE_CHECK(late_start.outcome == WatchOutcome::not_found, "a late watch finds nothing");
            RACE_CHECK(late->count(TaskChangeKind::removed) == 0u, "and is told nothing");
        }
        RACE_CHECK(!store->get(id, TaskOwner{}).has_value(), "the task is gone");
        RACE_CHECK(store->usage().tasks == 0u && store->usage().bytes == 0u, "nothing is left of it");
        late_start = TaskWatchStart{};
    }
    stop = true;
    start.arrive_and_wait();
    sweeper.join();
    poller.join();
    watcher.join();
    (void)poll_found;
}

// A task whose id has been handed to another thread is found there: create returns after the record is readable.
TEST(TaskStoreRaces, CreateIsVisibleToAnotherThreadOnceItReturns) {
    constexpr int count = 5000;
    MemoryTaskStoreOptions options;
    options.max_tasks = count;
    options.max_tasks_per_owner = count;
    auto store = make_memory_task_store(options);

    std::mutex mutex;
    std::vector<std::string> handed;
    std::atomic<bool> done{false};
    std::atomic<int> missing{0};
    std::atomic<int> checked{0};
    const auto consumer = [&] {
        std::size_t next = 0;
        while (true) {
            std::string id;
            {
                const std::lock_guard<std::mutex> lock(mutex);
                if (next < handed.size()) {
                    id = handed[next++];
                } else if (done.load()) {
                    return;
                }
            }
            if (id.empty()) {
                std::this_thread::yield();
                continue;
            }
            if (!store->get(id, TaskOwner{}).has_value()) ++missing;
            ++checked;
        }
    };
    std::thread a(consumer);
    std::thread b(consumer);
    for (int i = 0; i < count; ++i) {
        const TaskRecord record = make_task(*store);
        const std::lock_guard<std::mutex> lock(mutex);
        handed.push_back(record.id);
    }
    done = true;
    a.join();
    b.join();
    EXPECT_EQ(missing.load(), 0);
    EXPECT_EQ(checked.load(), 2 * count);
}

// Many owners fill a store from many threads: the totals never pass the bounds, at any moment a thread looks.
TEST(TaskStoreRaces, ConcurrentCreationNeverPassesTheBounds) {
    constexpr int threads = 8;
    constexpr int attempts = 200;
    MemoryTaskStoreOptions options;
    options.max_tasks = 100;
    options.max_tasks_per_owner = 20;
    options.max_stored_bytes = std::size_t{1} << 30;
    {
        auto probe = make_memory_task_store(options);
        make_task(*probe, draft("owner0"));  // the owners "owner0" to "owner7" all have names of one length
        // The bytes cap at 37 records, below both counts.
        options.max_stored_bytes = 37 * probe->usage().bytes;
        options.max_tasks = 1000;
        options.max_tasks_per_owner = 1000;
    }
    auto store = make_memory_task_store(options);

    std::atomic<bool> running{true};
    std::atomic<int> over{0};
    std::thread sampler([&] {
        while (running.load()) {
            const TaskStoreUsage usage = store->usage();
            if (usage.tasks > 37 || usage.bytes > options.max_stored_bytes) ++over;
        }
    });
    std::vector<std::thread> workers;
    std::atomic<int> created{0};
    std::atomic<int> refused_for_bytes{0};
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            for (int i = 0; i < attempts; ++i) {
                CreateResult result = store->create(draft("owner" + std::to_string(t)));
                if (std::holds_alternative<TaskRecord>(result)) {
                    ++created;
                } else if (std::get<CreateRefusal>(result) == CreateRefusal::byte_budget) {
                    ++refused_for_bytes;
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();
    running = false;
    sampler.join();
    EXPECT_EQ(over.load(), 0);
    EXPECT_EQ(created.load(), 37) << "the budget holds exactly 37 records";
    EXPECT_EQ(refused_for_bytes.load(), threads * attempts - 37);
    EXPECT_EQ(store->usage().tasks, 37u);
    EXPECT_LE(store->usage().bytes, options.max_stored_bytes);
}

TEST(TaskStoreRaces, ConcurrentCreationNeverPassesTheCountsAndEachOwnerKeepsItsShare) {
    constexpr int threads = 8;
    constexpr int attempts = 200;
    MemoryTaskStoreOptions options;
    options.max_tasks = 100;
    options.max_tasks_per_owner = 20;
    auto store = make_memory_task_store(options);

    std::atomic<bool> running{true};
    std::atomic<int> over{0};
    std::thread sampler([&] {
        while (running.load()) {
            if (store->usage().tasks > 100) ++over;
        }
    });
    std::vector<int> created(threads, 0);
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            for (int i = 0; i < attempts; ++i) {
                if (std::holds_alternative<TaskRecord>(store->create(draft("owner" + std::to_string(t))))) {
                    ++created[t];
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();
    running = false;
    sampler.join();
    EXPECT_EQ(over.load(), 0);
    int total = 0;
    for (const int own : created) {
        EXPECT_LE(own, 20) << "no owner passes its own bound";
        total += own;
    }
    EXPECT_EQ(total, 100) << "eight owners of 20 fill the 100 the store holds, and no more";
    EXPECT_EQ(store->usage().tasks, 100u);
}

std::optional<std::size_t> task_store_test_heap_in_use() {
#if defined(NEOMCP_TASK_TEST_SANITIZER_HEAP)
    return __sanitizer_get_current_allocated_bytes();
#elif defined(NEOMCP_TASK_TEST_MALLINFO2)
    const struct mallinfo2 info = mallinfo2();
    return info.uordblks + info.hblkhd;
#else
    return std::nullopt;
#endif
}

TEST(TaskStore, StoredRequestDoesNotPinLargeParsedDocument) {
    auto store = make_memory_task_store();
    // Warm the store and finish the warmup task so recover() only targets unfinished test tasks below.
    const TaskRecord warmup = make_task(*store);
    ASSERT_FALSE(warmup.id.empty());
    ASSERT_EQ(step(*store, warmup.id, TaskStatus::working, to(TaskStatus::completed)).outcome,
              TransitionOutcome::applied);
    neojson::trim_thread_caches();

    std::string id;
    std::string ans_id;
    std::string err_id;
    std::string rec_id;
    const std::optional<std::size_t> before = task_store_test_heap_in_use();
    {
        const auto extract_small = [](const char* key) -> json {
            const std::string payload =
                json{{key, {{"name", "tool"}, {"arguments", {{"x", 1}}}}},
                     {"discard", std::string(2u * 1024u * 1024u, 'z')}}
                    .dump();
            const json parsed = json::parse(payload);
            // Copying from a const parsed document yields a lazy COW subtree; the store must materialize it.
            return parsed.at(key);
        };

        TaskDraft task = draft();
        task.request = extract_small("keep");
        id = make_task(*store, std::move(task)).id;

        TaskUpdate done = to(TaskStatus::completed);
        done.result = extract_small("res");
        ASSERT_EQ(step(*store, id, TaskStatus::working, std::move(done)).outcome, TransitionOutcome::applied);

        ans_id = make_task(*store).id;
        TaskUpdate ask = to(TaskStatus::input_required);
        ask.asked = extract_small("asked");
        ASSERT_EQ(step(*store, ans_id, TaskStatus::working, std::move(ask)).outcome, TransitionOutcome::applied);
        {
            const std::string apayload =
                json{{"ans", json{{"name", "answered"}}}, {"discard", std::string(2u * 1024u * 1024u, 'q')}}.dump();
            const json aparsed = json::parse(apayload);
            ASSERT_EQ(store->put_answers(ans_id, TaskOwner{}, aparsed.at("ans")).outcome, AnswersOutcome::applied);
        }
        const auto loaded_ans = store->get(ans_id, TaskOwner{});
        ASSERT_TRUE(loaded_ans.has_value());
        EXPECT_EQ(loaded_ans->answers, (json{{"name", "answered"}}));

        err_id = make_task(*store).id;
        TaskUpdate fail = to(TaskStatus::failed);
        fail.error = extract_small("err");
        ASSERT_EQ(step(*store, err_id, TaskStatus::working, std::move(fail)).outcome, TransitionOutcome::applied);

        rec_id = make_task(*store).id;
        TaskUpdate rec = to(TaskStatus::failed);
        rec.error = extract_small("rec");
        // ans_id (input_required) and rec_id (working) are both unfinished.
        EXPECT_EQ(store->recover(rec), 2u);
    }
    neojson::trim_thread_caches();
    const std::optional<std::size_t> after = task_store_test_heap_in_use();

    const auto loaded = store->get(id, TaskOwner{});
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->request, (json{{"name", "tool"}, {"arguments", {{"x", 1}}}}));
    EXPECT_EQ(loaded->result, (json{{"name", "tool"}, {"arguments", {{"x", 1}}}}));
    const auto loaded_err = store->get(err_id, TaskOwner{});
    ASSERT_TRUE(loaded_err.has_value());
    EXPECT_EQ(loaded_err->error, (json{{"name", "tool"}, {"arguments", {{"x", 1}}}}));
    const auto loaded_rec = store->get(rec_id, TaskOwner{});
    ASSERT_TRUE(loaded_rec.has_value());
    EXPECT_EQ(loaded_rec->error, (json{{"name", "tool"}, {"arguments", {{"x", 1}}}}));

    if (before && after) {
        const std::size_t kept = (*after > *before) ? (*after - *before) : 0u;
        EXPECT_LT(kept, 1024u * 1024u) << "stored task records pinned " << kept << " extra bytes";
    }
}

}  // namespace
