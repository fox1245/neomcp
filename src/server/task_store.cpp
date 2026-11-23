#include "neomcp/server/task_store.h"

#include <algorithm>
#include <charconv>
#include <map>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

#include "core/large_json_cache.h"
#include "neomcp/random.h"

namespace neomcp {

namespace {

using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

// `start` plus `span`, or the latest time there is when the sum would not fit.
TimePoint saturating_add(TimePoint start, std::chrono::milliseconds span) {
    const auto room = (TimePoint::max)() - start;
    if (span >= std::chrono::duration_cast<std::chrono::milliseconds>(room)) return (TimePoint::max)();
    return start + span;
}

std::size_t serialized_size(const json& value) {
    detail::LargeJsonCacheGuard trim_caches(true);
    const std::size_t size = value.dump().size();
    trim_caches.active = size >= detail::large_json_trim_threshold;
    return size;
}

std::size_t serialized_size(const std::optional<json>& value) { return value ? serialized_size(*value) : 0; }

// The listeners to tell once the store's lock is released, with what to tell them.
class Deliveries {
public:
    void add(const std::map<std::uint64_t, std::weak_ptr<TaskListener>>& listeners, const TaskChange& change) {
        for (const auto& entry : listeners) pending_.emplace_back(entry.second, change);
    }

    void deliver() {
        for (const auto& [weak, change] : pending_) {
            const std::shared_ptr<TaskListener> listener = weak.lock();
            if (!listener) continue;
            try {
                listener->on_task_change(change);
            } catch (...) {
                // A listener that throws must not stop the others or the call that made the change.
            }
        }
        pending_.clear();
    }

private:
    std::vector<std::pair<std::weak_ptr<TaskListener>, TaskChange>> pending_;
};

// One task and what the store keeps beside it.
struct Entry {
    TaskRecord record;
    std::uint64_t sequence = 0;  // the order of creation, across all owners
    TimePoint expires{};
    // The sizes, in bytes, that the store counts for the record; `accounted` is their sum, with the reserve for the
    // outcome while the task is running.
    std::size_t fixed = 0;
    std::size_t request = 0;
    std::size_t message = 0;
    std::size_t result = 0;
    std::size_t error = 0;
    std::size_t asked = 0;
    std::size_t answers = 0;
    std::size_t accounted = 0;
    std::map<std::uint64_t, std::weak_ptr<TaskListener>> watchers;
};

class MemoryTaskStore final : public TaskStore, public std::enable_shared_from_this<MemoryTaskStore> {
public:
    explicit MemoryTaskStore(MemoryTaskStoreOptions options) : options_(std::move(options)) {}

    CreateResult create(TaskDraft draft) override {
        // Detach any shared parsed document backing before storing a potentially long-lived record.
        draft.request.materialize();
        Deliveries deliveries;
        CreateResult result = CreateRefusal::store_full;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            const TimePoint now = clock();
            sweep(now, deliveries, nullptr);
            result = create_locked(std::move(draft), now);
        }
        deliveries.deliver();
        return result;
    }

    std::optional<TaskRecord> get(std::string_view id, const TaskOwner& owner) const override {
        const std::lock_guard<std::mutex> lock(mutex_);
        const Entry* entry = visible(id, owner, clock());
        if (entry == nullptr) return std::nullopt;
        return entry->record;
    }

    Transition transition(std::string_view id, std::span<const TaskStatus> from, TaskUpdate update) override {
        if (update.result) update.result->materialize();
        if (update.error) update.error->materialize();
        if (update.asked) update.asked->materialize();
        Deliveries deliveries;
        Transition result;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            const TimePoint now = clock();
            Entry* entry = find_live(id, now, deliveries);
            if (entry == nullptr) {
                result.outcome = TransitionOutcome::not_found;
            } else if (is_terminal(entry->record.status) ||
                       std::find(from.begin(), from.end(), entry->record.status) == from.end()) {
                result.outcome = TransitionOutcome::status_mismatch;
                result.record = entry->record;
            } else {
                result.outcome = apply(*entry, std::move(update), now, true, deliveries);
                result.record = entry->record;
            }
        }
        deliveries.deliver();
        return result;
    }

    AnswersResult put_answers(std::string_view id, const TaskOwner& owner, json answers) override {
        answers.materialize();
        Deliveries deliveries;
        AnswersResult result;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            const TimePoint now = clock();
            Entry* entry = find_live(id, now, deliveries);
            if (entry == nullptr || !(entry->record.owner == owner)) {
                result.outcome = AnswersOutcome::not_found;
            } else if (entry->record.status != TaskStatus::input_required || !entry->record.asked ||
                       !entry->record.asked->is_object()) {
                result.outcome = AnswersOutcome::not_waiting;
                result.record = entry->record;
            } else {
                result.outcome = keep_answers(*entry, answers, now, deliveries);
                result.record = entry->record;
            }
        }
        deliveries.deliver();
        return result;
    }

    std::optional<json> take_answers(std::string_view id) override {
        Deliveries deliveries;
        std::optional<json> taken;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            const TimePoint now = clock();
            Entry* entry = find_live(id, now, deliveries);
            if (entry != nullptr && entry->record.status == TaskStatus::input_required && entry->record.asked &&
                entry->record.asked->is_object() && !entry->record.asked->empty() && entry->record.answers &&
                answered_everything(*entry)) {
                taken = std::move(*entry->record.answers);
                entry->record.answers.reset();
                entry->record.asked.reset();
                entry->record.status = TaskStatus::working;
                entry->record.updated = now;
                recount(*entry);
                notify(*entry, deliveries);
            }
        }
        deliveries.deliver();
        return taken;
    }

    std::optional<TaskPage> list(const TaskOwner& owner, std::optional<std::string_view> cursor,
                                 std::size_t limit) const override {
        std::uint64_t after = 0;
        bool from_start = true;
        if (cursor) {
            const std::string_view text = *cursor;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), after);
            if (text.empty() || parsed.ec != std::errc() || parsed.ptr != text.data() + text.size()) {
                return std::nullopt;
            }
            from_start = false;
        }
        const std::size_t size = limit == 0 || limit > options_.max_page_size ? options_.max_page_size : limit;

        const std::lock_guard<std::mutex> lock(mutex_);
        const TimePoint now = clock();
        TaskPage page;
        const auto owned = by_owner_.find(owner.principal);
        if (owned == by_owner_.end()) return page;
        auto it = from_start ? owned->second.begin() : owned->second.upper_bound(after);
        std::uint64_t last = 0;
        for (; it != owned->second.end(); ++it) {
            const Entry& entry = entries_.at(it->second);
            if (!(now < entry.expires)) continue;  // ended, and not swept yet
            if (page.tasks.size() == size) {
                page.next_cursor = std::to_string(last);
                break;
            }
            TaskRecord summary = entry.record;
            summary.request = nullptr;
            summary.result.reset();
            summary.error.reset();
            summary.asked.reset();
            summary.answers.reset();
            page.tasks.push_back(std::move(summary));
            last = entry.sequence;
        }
        return page;
    }

    std::vector<ExpiredTask> expire(TimePoint now) override {
        Deliveries deliveries;
        std::vector<ExpiredTask> gone;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            sweep(now, deliveries, &gone);
        }
        deliveries.deliver();
        return gone;
    }

    std::size_t recover(const TaskUpdate& failure) override {
        const std::size_t outcome_bytes = serialized_size(failure.result) + serialized_size(failure.error) +
                                          serialized_size(failure.asked) + (failure.message ? failure.message->size() : 0);
        const std::size_t largest = std::max({serialized_size(failure.result), serialized_size(failure.error),
                                              serialized_size(failure.asked)});
        if (!is_terminal(failure.status) || outcome_bytes > options_.outcome_reserve_bytes ||
            largest > options_.max_result_bytes) {
            throw std::invalid_argument(
                "neomcp: a recovery is a terminal state with an outcome no larger than the reserve");
        }
        TaskUpdate detached = failure;
        if (detached.result) detached.result->materialize();
        if (detached.error) detached.error->materialize();
        if (detached.asked) detached.asked->materialize();
        Deliveries deliveries;
        std::size_t changed = 0;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            const TimePoint now = clock();
            for (auto& [id, entry] : entries_) {
                if (is_terminal(entry.record.status) || !(now < entry.expires)) continue;
                if (apply(entry, detached, now, false, deliveries) == TransitionOutcome::applied) ++changed;
            }
        }
        deliveries.deliver();
        return changed;
    }

    TaskWatchStart watch(std::string_view id, const TaskOwner& owner, std::weak_ptr<TaskListener> listener) override {
        Deliveries deliveries;
        TaskWatchStart start;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            const TimePoint now = clock();
            Entry* entry = find_live(id, now, deliveries);
            if (entry == nullptr || !(entry->record.owner == owner)) {
                start.outcome = WatchOutcome::not_found;
            } else if (watches_ >= options_.max_watches) {
                start.outcome = WatchOutcome::limit;
            } else {
                const std::uint64_t key = ++last_watch_;
                entry->watchers.emplace(key, std::move(listener));
                ++watches_;
                start.outcome = WatchOutcome::watching;
                start.record = entry->record;
                start.watch = TaskWatch([weak = weak_from_this(), task = std::string(id), key] {
                    if (const auto store = weak.lock()) store->unwatch(task, key);
                });
            }
        }
        deliveries.deliver();
        return start;
    }

    TaskStoreUsage usage() const override {
        const std::lock_guard<std::mutex> lock(mutex_);
        return TaskStoreUsage{entries_.size(), bytes_};
    }

private:
    TimePoint clock() const { return options_.clock ? options_.clock() : Clock::now(); }

    // The record of a task that exists, has not ended its lifetime and belongs to `owner`.
    const Entry* visible(std::string_view id, const TaskOwner& owner, TimePoint now) const {
        const auto it = entries_.find(std::string(id));
        if (it == entries_.end() || !(now < it->second.expires) || !(it->second.record.owner == owner)) return nullptr;
        return &it->second;
    }

    // The entry of a task that exists. One whose lifetime ended is removed (and its watchers told) and is none.
    Entry* find_live(std::string_view id, TimePoint now, Deliveries& deliveries) {
        const auto it = entries_.find(std::string(id));
        if (it == entries_.end()) return nullptr;
        if (!(now < it->second.expires)) {
            sweep(now, deliveries, nullptr);
            return nullptr;
        }
        return &it->second;
    }

    void unwatch(const std::string& id, std::uint64_t key) {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(id);
        if (it == entries_.end()) return;
        if (it->second.watchers.erase(key) != 0) --watches_;
    }

    // Removes the tasks whose lifetime ended by `now`, telling their watchers.
    void sweep(TimePoint now, Deliveries& deliveries, std::vector<ExpiredTask>* gone) {
        while (!expiry_.empty() && !(now < expiry_.begin()->first)) {
            const std::string id = expiry_.begin()->second;
            expiry_.erase(expiry_.begin());
            const auto it = entries_.find(id);
            if (it == entries_.end()) continue;
            Entry& entry = it->second;
            deliveries.add(entry.watchers, TaskChange{id, TaskChangeKind::removed, entry.record.status});
            watches_ -= entry.watchers.size();
            if (gone != nullptr) gone->push_back(ExpiredTask{id, entry.record.status});
            bytes_ -= entry.accounted;
            auto& owned = by_owner_[entry.record.owner.principal];
            owned.erase(entry.sequence);
            if (owned.empty()) by_owner_.erase(entry.record.owner.principal);
            entries_.erase(it);
        }
    }

    std::optional<std::string> draw_id() {
        for (std::size_t attempt = 0; attempt < options_.id_attempts; ++attempt) {
            std::optional<std::string> id =
                options_.new_id ? options_.new_id() : random_hex(TaskStoreDefaults::id_bytes);
            if (!id || id->empty()) return std::nullopt;
            if (entries_.find(*id) == entries_.end()) return id;
        }
        return std::nullopt;
    }

    CreateResult create_locked(TaskDraft draft, TimePoint now) {
        if (entries_.size() >= options_.max_tasks) return CreateRefusal::store_full;
        const auto owned = by_owner_.find(draft.owner.principal);
        if (owned != by_owner_.end() && owned->second.size() >= options_.max_tasks_per_owner) {
            return CreateRefusal::owner_full;
        }

        std::optional<std::string> id = draw_id();
        if (!id) return CreateRefusal::id_unavailable;

        Entry entry;
        entry.request = serialized_size(draft.request);
        entry.fixed = id->size() + draft.owner.principal.size() + draft.revision.size();
        entry.accounted = entry.fixed + entry.request + options_.outcome_reserve_bytes;
        if (!fits(entry.accounted)) return CreateRefusal::byte_budget;

        const std::chrono::milliseconds ttl =
            std::clamp(draft.ttl.value_or(options_.default_ttl), options_.min_ttl, options_.max_ttl);
        TaskRecord& record = entry.record;
        record.id = *id;
        record.owner = std::move(draft.owner);
        record.status = TaskStatus::working;
        record.created = now;
        record.updated = now;
        record.ttl = ttl;
        record.poll_interval = options_.poll_interval;
        record.revision = std::move(draft.revision);
        record.request = std::move(draft.request);
        entry.expires = saturating_add(now, ttl);
        entry.sequence = ++last_sequence_;

        bytes_ += entry.accounted;
        by_owner_[record.owner.principal].emplace(entry.sequence, record.id);
        expiry_.emplace(entry.expires, record.id);
        TaskRecord created = record;
        entries_.emplace(record.id, std::move(entry));
        return created;
    }

    // Recomputes what the record is counted at, from its parts, and moves the store's total by the difference.
    void recount(Entry& entry) {
        entry.message = entry.record.status_message ? entry.record.status_message->size() : 0;
        entry.result = serialized_size(entry.record.result);
        entry.error = serialized_size(entry.record.error);
        entry.asked = serialized_size(entry.record.asked);
        entry.answers = serialized_size(entry.record.answers);
        const std::size_t reserve = is_terminal(entry.record.status) ? 0 : options_.outcome_reserve_bytes;
        const std::size_t now_counted = entry.fixed + entry.request + entry.message + entry.result + entry.error +
                                        entry.asked + entry.answers + reserve;
        bytes_ = bytes_ - entry.accounted + now_counted;
        entry.accounted = now_counted;
    }

    static void notify(const Entry& entry, Deliveries& deliveries) {
        deliveries.add(entry.watchers, TaskChange{entry.record.id, TaskChangeKind::updated, entry.record.status});
    }

    bool fits(std::size_t grows_by) const {
        return grows_by == 0 || grows_by <= options_.max_stored_bytes - std::min(options_.max_stored_bytes, bytes_);
    }

    TransitionOutcome apply(Entry& entry, TaskUpdate update, TimePoint now, bool check_budget, Deliveries& deliveries) {
        const std::size_t largest = options_.max_result_bytes;
        const std::size_t result = serialized_size(update.result);
        const std::size_t error = serialized_size(update.error);
        const std::size_t asked = serialized_size(update.asked);
        if (result > largest || error > largest || asked > largest) return TransitionOutcome::too_large;

        // What the record would be counted at afterwards.
        const bool keeps_input = update.status == TaskStatus::input_required;
        const std::size_t message = update.message ? update.message->size() : 0;
        const std::size_t new_result = update.result ? result : entry.result;
        const std::size_t new_error = update.error ? error : entry.error;
        const std::size_t new_asked = !keeps_input ? 0 : (update.asked ? asked : entry.asked);
        const std::size_t new_answers = (!keeps_input || update.asked) ? 0 : entry.answers;
        const std::size_t reserve = is_terminal(update.status) ? 0 : options_.outcome_reserve_bytes;
        const std::size_t after = entry.fixed + entry.request + message + new_result + new_error + new_asked +
                                  new_answers + reserve;
        if (check_budget && after > entry.accounted && !fits(after - entry.accounted)) {
            return TransitionOutcome::over_budget;
        }

        TaskRecord& record = entry.record;
        record.status = update.status;
        record.status_message = std::move(update.message);
        if (update.result) record.result = std::move(update.result);
        if (update.error) record.error = std::move(update.error);
        if (!keeps_input) {
            record.asked.reset();
            record.answers.reset();
        } else if (update.asked) {
            record.asked = std::move(update.asked);
            record.answers.reset();
        }
        record.updated = now;
        recount(entry);
        notify(entry, deliveries);
        return TransitionOutcome::applied;
    }

    AnswersOutcome keep_answers(Entry& entry, const json& answers, TimePoint now, Deliveries& deliveries) {
        json merged = entry.record.answers && entry.record.answers->is_object() ? *entry.record.answers : json::object();
        if (answers.is_object()) {
            for (const auto& item : answers.items()) {
                if (entry.record.asked->contains(item.key()) && !merged.contains(item.key())) {
                    merged[item.key()] = item.value();
                }
            }
        }
        const std::size_t size = serialized_size(merged);
        if (size > options_.max_result_bytes) return AnswersOutcome::too_large;
        if (size > entry.answers && !fits(size - entry.answers)) return AnswersOutcome::over_budget;
        entry.record.answers = std::move(merged);
        entry.record.updated = now;
        recount(entry);
        notify(entry, deliveries);
        return AnswersOutcome::applied;
    }

    static bool answered_everything(const Entry& entry) {
        if (!entry.record.answers->is_object()) return false;
        for (const auto& item : entry.record.asked->items()) {
            if (!entry.record.answers->contains(item.key())) return false;
        }
        return true;
    }

    const MemoryTaskStoreOptions options_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
    std::unordered_map<std::string, std::map<std::uint64_t, std::string>> by_owner_;  // principal: sequence -> id
    std::multimap<TimePoint, std::string> expiry_;                                    // when a task ends -> id
    std::size_t bytes_ = 0;
    std::size_t watches_ = 0;
    std::uint64_t last_sequence_ = 0;
    std::uint64_t last_watch_ = 0;
};

void require(bool ok, const char* what) {
    if (!ok) throw std::invalid_argument(std::string("neomcp: MemoryTaskStoreOptions::") + what);
}

}  // namespace

std::shared_ptr<TaskStore> make_memory_task_store(MemoryTaskStoreOptions options) {
    const std::chrono::milliseconds none{0};
    require(options.max_tasks > 0, "max_tasks is zero");
    require(options.max_tasks_per_owner > 0, "max_tasks_per_owner is zero");
    require(options.default_ttl > none, "default_ttl is not positive");
    require(options.min_ttl > none, "min_ttl is not positive");
    require(options.max_ttl > none, "max_ttl is not positive");
    require(options.min_ttl <= options.max_ttl, "min_ttl is above max_ttl");
    require(options.default_ttl >= options.min_ttl && options.default_ttl <= options.max_ttl,
            "default_ttl is outside min_ttl and max_ttl");
    require(options.poll_interval > none, "poll_interval is not positive");
    require(options.max_result_bytes > 0, "max_result_bytes is zero");
    require(options.max_stored_bytes > 0, "max_stored_bytes is zero");
    require(options.max_page_size > 0, "max_page_size is zero");
    require(options.max_watches > 0, "max_watches is zero");
    require(options.id_attempts > 0, "id_attempts is zero");
    return std::make_shared<MemoryTaskStore>(std::move(options));
}

}  // namespace neomcp
