#include "raftpp/memory_storage.h"

#include <libassert/assert.hpp>

#include "raftpp/util.h"

namespace raftpp {

MemoryStorageCore::MemoryStorageCore()
    : trigger_snapshot_unavailable_(false), trigger_log_unavailable_(false) {}

void MemoryStorageCore::SetHardState(HardState&& hs) {
    static_assert(std::is_rvalue_reference_v<decltype(hs)>, "hs must be rvalue reference");
    raft_state_.hard_state = std::move(hs);
}

void MemoryStorageCore::CommitTo(uint64_t index) {
    ASSERT(HasEntryAt(index), "commit_to {} but the entry does not exist", index);
    const size_t diff = index - entries_[0].index();
    raft_state_.hard_state.set_commit(index);
    raft_state_.hard_state.set_term(entries_[diff].term());
}

bool MemoryStorageCore::HasEntryAt(const uint64_t index) const {
    return !entries_.empty() && index >= first_index() && index <= last_index();
}

Result<void> MemoryStorageCore::ApplySnapshot(const Snapshot& snapshot) {
    const auto& meta = snapshot.metadata();
    const uint64_t index = meta.index();
    if (first_index() > index) {
        return RaftError(StorageErrorCode::SnapshotOutOfDate);
    }

    snapshot_metadata_.CopyFrom(meta);
    raft_state_.hard_state.set_term(std::max(raft_state_.hard_state.term(), meta.term()));
    raft_state_.hard_state.set_commit(index);
    entries_.clear();
    raft_state_.conf_state.CopyFrom(meta.conf_state());
    return {};
}

Result<void> MemoryStorageCore::Compact(uint64_t compact_index) {
    if (compact_index <= first_index()) {
        return {};
    }

    if (compact_index > last_index() + 1) {
        return RaftError(
            FatalError{fmt::format(
                "compact not received raft logs, compact_index={} last_index={}", compact_index,
                last_index()
            )}
        );
    }

    if (entries_.empty()) {
        return {};
    }

    const uint64_t offset = compact_index - entries_[0].index();
    entries_.erase(entries_.begin(), entries_.begin() + static_cast<int64_t>(offset));
    return {};
}

Result<void> MemoryStorageCore::Append(const std::vector<Entry>& ents) {
    return MayAppend(ents);
}

Result<void> MemoryStorageCore::MayAppend(const std::vector<Entry>& ents) {
    if (ents.empty()) {
        return {};
    }

    const auto new_appended = ents.front().index();
    if (first_index() > new_appended) {
        const auto compacted = first_index() - 1;
        return RaftError(
            FatalError{fmt::format(
                "overwrite compacted raft logs, compacted={} new_appended={}", compacted,
                new_appended
            )}
        );
    }

    if (last_index() + 1 < new_appended) {
        return RaftError(
            FatalError{fmt::format(
                "raft logs should be continuous, last_index={} new_appended={}", last_index(),
                new_appended
            )}
        );
    }

    if (const uint64_t diff = new_appended - first_index(); diff < entries_.size()) {
        entries_.erase(entries_.begin() + static_cast<int64_t>(diff), entries_.end());
    }
    entries_.reserve(entries_.size() + ents.size());
    entries_.insert_range(entries_.end(), ents);
    return {};
}

void MemoryStorageCore::TriggerSnapshotUnavailable() {
    trigger_snapshot_unavailable_ = true;
}

void MemoryStorageCore::TriggerLogUnavailable() {
    trigger_log_unavailable_ = true;
}

std::optional<GetEntriesContext> MemoryStorageCore::TakeGetEntriesContext() {
    const auto ctx = get_entries_context_;
    get_entries_context_ = std::nullopt;
    return ctx;
}

uint64_t MemoryStorageCore::first_index() const {
    if (entries_.empty()) {
        return snapshot_metadata_.index() + 1;
    }
    return entries_[0].index();
}

uint64_t MemoryStorageCore::last_index() const {
    if (entries_.empty()) {
        return snapshot_metadata_.index();
    }
    return entries_.back().index();
}

Snapshot MemoryStorageCore::snapshot() const {
    Snapshot snapshot;

    auto* meta = snapshot.mutable_metadata();
    meta->set_index(raft_state_.hard_state.commit());

    uint64_t term;
    if (meta->index() < snapshot_metadata_.index()) {
        PANIC("commit {} < snapshot_metadata.index {}", meta->index(), snapshot_metadata_.index());
    }
    if (meta->index() > snapshot_metadata_.index()) {
        const uint64_t offset = entries_[0].index();
        term = entries_[(meta->index() - offset)].term();
    } else {
        term = snapshot_metadata_.term();
    }

    meta->set_term(term);
    meta->mutable_conf_state()->CopyFrom(raft_state_.conf_state);

    return snapshot;
}

MemoryStorage::~MemoryStorage() = default;

Result<RaftState> MemoryStorage::InitialState() {
    std::lock_guard lock(mutex_);
    return core_.raft_state_;
}

Result<std::vector<Entry>> MemoryStorage::Entries(
    const uint64_t low, uint64_t high, const std::optional<uint64_t> max_size,
    GetEntriesContext context
) {
    std::lock_guard lock(mutex_);

    if (low < core_.first_index()) {
        return RaftError(StorageErrorCode::Compacted);
    }

    if (high > core_.last_index() + 1) {
        PANIC("index out of bound (last: {}, high: {})", core_.last_index() + 1, high);
    }

    if (core_.trigger_log_unavailable_ && context.CanAsync()) {
        core_.get_entries_context_ = context;
        return RaftError(StorageErrorCode::LogTemporarilyUnavailable);
    }

    const uint64_t offset = core_.entries_.front().index();
    const auto lo = static_cast<int64_t>(low - offset);
    const auto hi = static_cast<int64_t>(high - offset);
    std::vector<Entry> entries;
    for (auto it = core_.entries_.begin() + lo; it != core_.entries_.begin() + hi; ++it) {
        entries.emplace_back(*it);
    }
    if (max_size) {
        LimitSize(entries, *max_size);
    }
    return entries;
}

void MemoryStorage::SetEntries(const std::vector<Entry>& entries) {
    std::lock_guard lock(mutex_);
    core_.entries_ = entries;
}

Result<void> MemoryStorage::Append(const std::vector<Entry>& ents) {
    std::lock_guard lock(mutex_);
    return core_.Append(ents);
}

Result<void> MemoryStorage::Compact(const uint64_t idx) {
    std::lock_guard lock(mutex_);
    return core_.Compact(idx);
}

void MemoryStorage::SetRaftState(const RaftState& raft_state) {
    std::lock_guard lock(mutex_);
    core_.raft_state_ = raft_state;
}

void MemoryStorage::TriggerSnapshotUnavailable() {
    std::lock_guard lock(mutex_);
    core_.TriggerSnapshotUnavailable();
}

Result<void> MemoryStorage::ApplySnapshot(const Snapshot& snapshot) {
    std::lock_guard lock(mutex_);
    return core_.ApplySnapshot(snapshot);
}

std::vector<Entry> MemoryStorage::AllEntries() {
    std::lock_guard lock(mutex_);
    return core_.entries_;
}

Result<void> MemoryStorage::MayAppend(const std::vector<Entry>& entries) {
    std::lock_guard lock(mutex_);
    return core_.MayAppend(entries);
}

Result<uint64_t> MemoryStorage::Term(const uint64_t idx) {
    std::lock_guard lock(mutex_);
    if (idx == core_.snapshot_metadata_.index()) {
        return core_.snapshot_metadata_.term();
    }

    const auto offset = core_.first_index();
    if (idx < offset) {
        return RaftError(StorageErrorCode::Compacted);
    }

    if (idx > core_.last_index()) {
        return RaftError(StorageErrorCode::Unavailable);
    }

    return core_.entries_[idx - offset].term();
}

Result<uint64_t> MemoryStorage::FirstIndex() {
    std::lock_guard lock(mutex_);
    return core_.first_index();
}

Result<uint64_t> MemoryStorage::LastIndex() {
    std::lock_guard lock(mutex_);
    return core_.last_index();
}

Result<Snapshot> MemoryStorage::GetSnapshot(const uint64_t request_index, uint64_t to) {
    std::lock_guard lock(mutex_);
    if (core_.trigger_snapshot_unavailable_) {
        core_.trigger_snapshot_unavailable_ = false;
        return RaftError(StorageErrorCode::SnapshotTemporarilyUnavailable);
    }

    Snapshot snapshot = core_.snapshot();
    if (snapshot.metadata().index() < request_index) {
        snapshot.mutable_metadata()->set_index(request_index);
    }
    return snapshot;
}

}  // namespace raftpp
