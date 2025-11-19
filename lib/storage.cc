#include "raftpp/storage.h"

#include <libassert/assert.hpp>
#include <spdlog/fmt/fmt.h>

#include "raftpp/util.h"

namespace raftpp {

MemoryStorageCore::MemoryStorageCore() : trigger_snapshot_unavailable_(false), trigger_log_unavailable_(false) {}

bool GetEntriesContext::CanAsync() const {
    if (what == GetEntriesFor::Empty) {
        return payload.empty.can_async;
    }
    if (what == GetEntriesFor::SendAppend) {
        return true;
    }
    return false;
}

GetEntriesContext GetEntriesContext::Empty(const bool can_async) {
    return GetEntriesContext{
        .what = GetEntriesFor::Empty, .payload = GetEntriesForPayload{.empty = {.can_async = can_async}}
    };
}

void MemoryStorageCore::SetHardState(HardState hs) {
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

Result<void, StorageErrorCode> MemoryStorageCore::ApplySnapshot(const Snapshot& snapshot) {
    const auto meta = snapshot.metadata();
    const uint64_t index = meta.index();
    if (first_index() > index) {
        return std::unexpected(StorageErrorCode::SnapshotOutOfDate);
    }

    snapshot_metadata_.CopyFrom(meta);
    raft_state_.hard_state.set_term(std::max(raft_state_.hard_state.term(), meta.term()));
    raft_state_.hard_state.set_commit(index);
    entries_.clear();
    raft_state_.conf_state.CopyFrom(meta.conf_state());
    return {};
}

void MemoryStorageCore::Compact(uint64_t compact_index) {
    if (compact_index >= first_index()) {
        return;
    }

    if (compact_index > last_index() + 1) {
        PANIC("compact not received raft logs: {}, last index: {}", compact_index, last_index());
    }

    if (entries_.empty()) {
        return;
    }

    const uint64_t offset = compact_index - entries_[0].index();
    entries_.erase(entries_.begin(), entries_.begin() + offset);
}

void MemoryStorageCore::Append(const std::vector<Entry>& ents) {
    if (ents.empty()) {
        return;
    }

    if (first_index() > ents.front().index()) {
        PANIC("overwrite compacted raft logs, compacted: {}, append: {}", first_index() - 1, ents[0].index());
    }

    if (last_index() + 1 > ents.front().index()) {
        PANIC("raft logs should be continuous, last index: {}, new appended: {}", last_index(), ents.front().index());
    }

    const uint64_t diff = ents.front().index() - first_index();
    entries_.erase(entries_.begin() + diff, entries_.end());
    entries_.reserve(entries_.size() + ents.size());
    entries_.insert(entries_.end(), ents.begin(), ents.end());
}

void MemoryStorageCore::TriggerSnapshotUnavailable() {
    trigger_snapshot_unavailable_ = true;
}

void MemoryStorageCore::TriggerLogUnavailable() {
    trigger_log_unavailable_ = true;
}

std::optional<GetEntriesContext> MemoryStorageCore::TakeGetEntriesContext() {
    const auto ctx = std::move(get_entries_context_);
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

MemoryStorage::MemoryStorage() {}

Result<RaftState, StorageErrorCode> MemoryStorage::InitialState() {
    std::lock_guard lock(mutex_);
    return core_.raft_state_;
}

Result<std::vector<Entry>, StorageErrorCode> MemoryStorage::Entries(
    const uint64_t low, uint64_t high, const std::optional<uint64_t> max_size, GetEntriesContext context
) {
    std::lock_guard lock(mutex_);

    if (low < core_.first_index()) {
        return std::unexpected(StorageErrorCode::Compacted);
    }

    if (high > core_.last_index() + 1) {
        PANIC("index out of bound (last: {}, high: {})", core_.last_index() + 1, high);
    }

    if (core_.trigger_log_unavailable_ && context.CanAsync()) {
        core_.get_entries_context_ = context;
        return std::unexpected(StorageErrorCode::LogTemporarilyUnavailable);
    }

    const auto offset = core_.entries_.front().index();
    const size_t lo = low - offset;
    const size_t hi = high - offset;
    std::vector<Entry> entries;
    for (auto it = core_.entries_.begin() + lo; it != core_.entries_.begin() + hi; ++it) {
        entries.emplace_back(*it);
    }
    LimitSize(entries, *max_size);
    return entries;
}

Result<uint64_t, StorageErrorCode> MemoryStorage::Term(const uint64_t idx) {
    std::lock_guard lock(mutex_);
    if (idx == core_.snapshot_metadata_.index()) {
        return core_.snapshot_metadata_.term();
    }

    const auto offset = core_.first_index();
    if (idx < offset) {
        return std::unexpected(StorageErrorCode::Compacted);
    }

    if (idx > core_.last_index()) {
        return std::unexpected(StorageErrorCode::Unavailable);
    }

    return core_.entries_[idx - offset].term();
}

Result<uint64_t, StorageErrorCode> MemoryStorage::FirstIndex() {
    std::lock_guard lock(mutex_);
    return core_.first_index();
}

Result<uint64_t, StorageErrorCode> MemoryStorage::LastIndex() {
    std::lock_guard lock(mutex_);
    return core_.last_index();
}

Result<Snapshot, StorageErrorCode> MemoryStorage::GetSnapshot(uint64_t request_index, uint64_t to) {
    std::lock_guard lock(mutex_);
    if (core_.trigger_snapshot_unavailable_) {
        core_.trigger_snapshot_unavailable_ = false;
        return std::unexpected(StorageErrorCode::SnapshotTemporarilyUnavailable);
    }

    Snapshot snapshot = core_.snapshot();
    if (snapshot.metadata().index() < request_index) {
        snapshot.mutable_metadata()->set_index(request_index);
    }
    return snapshot;
}

}  // namespace raftpp
