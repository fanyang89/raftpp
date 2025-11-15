#include "raftpp/core/memory_storage.h"

#include <libassert/assert.hpp>

#include "raftpp/core/util.h"

namespace raftpp {

MemoryStorageCore::MemoryStorageCore()
    : trigger_snapshot_unavailable_(false), trigger_log_unavailable_(false) {}

void MemoryStorageCore::SetHardState(HardState&& hs) {
    static_assert(std::is_rvalue_reference_v<decltype(hs)>, "hs must be rvalue reference");
    raft_state_.hard_state = std::move(hs);
}

void MemoryStorageCore::CommitTo(uint64_t index) {
    ASSERT(HasEntryAt(index), "commit_to {} but the entry does not exist", index);
    const size_t diff = index - entries_[0].reader().getIndex();
    raft_state_.hard_state.builder().setCommit(index);
    raft_state_.hard_state.builder().setTerm(entries_[diff].reader().getTerm());
}

bool MemoryStorageCore::HasEntryAt(const uint64_t index) const {
    return !entries_.empty() && index >= first_index() && index <= last_index();
}

Result<void> MemoryStorageCore::ApplySnapshot(const Snapshot& snapshot) {
    auto snapshot_reader = snapshot.reader();
    auto meta = snapshot_reader.getMetadata();
    const uint64_t index = meta.getIndex();
    if (first_index() > index) {
        return RaftError(StorageErrorCode::SnapshotOutOfDate);
    }

    // Copy snapshot metadata
    snapshot_metadata_ = SnapshotMetadata();
    auto meta_builder = snapshot_metadata_.builder();
    meta_builder.setIndex(meta.getIndex());
    meta_builder.setTerm(meta.getTerm());
    auto meta_conf = meta_builder.initConfState();
    auto src_conf = meta.getConfState();
    // Copy conf state voters
    auto voters = src_conf.getVoters();
    auto voters_builder = meta_conf.initVoters(voters.size());
    for (size_t i = 0; i < voters.size(); ++i) {
        voters_builder.set(i, voters[i]);
    }
    // Copy learners
    auto learners = src_conf.getLearners();
    auto learners_builder = meta_conf.initLearners(learners.size());
    for (size_t i = 0; i < learners.size(); ++i) {
        learners_builder.set(i, learners[i]);
    }
    // Copy voters_outgoing
    auto voters_outgoing = src_conf.getVotersOutgoing();
    auto voters_outgoing_builder = meta_conf.initVotersOutgoing(voters_outgoing.size());
    for (size_t i = 0; i < voters_outgoing.size(); ++i) {
        voters_outgoing_builder.set(i, voters_outgoing[i]);
    }
    // Copy learners_next
    auto learners_next = src_conf.getLearnersNext();
    auto learners_next_builder = meta_conf.initLearnersNext(learners_next.size());
    for (size_t i = 0; i < learners_next.size(); ++i) {
        learners_next_builder.set(i, learners_next[i]);
    }
    meta_conf.setAutoLeave(src_conf.getAutoLeave());

    auto hard_state_builder = raft_state_.hard_state.builder();
    auto current_term = hard_state_builder.getTerm();
    hard_state_builder.setTerm(std::max(current_term, meta.getTerm()));
    hard_state_builder.setCommit(index);
    entries_.clear();

    // Copy conf state to raft_state_
    raft_state_.conf_state = ConfState();
    auto conf_builder = raft_state_.conf_state.builder();
    auto conf_voters = conf_builder.initVoters(voters.size());
    for (size_t i = 0; i < voters.size(); ++i) {
        conf_voters.set(i, voters[i]);
    }
    auto conf_learners = conf_builder.initLearners(learners.size());
    for (size_t i = 0; i < learners.size(); ++i) {
        conf_learners.set(i, learners[i]);
    }
    auto conf_voters_outgoing = conf_builder.initVotersOutgoing(voters_outgoing.size());
    for (size_t i = 0; i < voters_outgoing.size(); ++i) {
        conf_voters_outgoing.set(i, voters_outgoing[i]);
    }
    auto conf_learners_next = conf_builder.initLearnersNext(learners_next.size());
    for (size_t i = 0; i < learners_next.size(); ++i) {
        conf_learners_next.set(i, learners_next[i]);
    }
    conf_builder.setAutoLeave(src_conf.getAutoLeave());
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

    const uint64_t offset = compact_index - entries_[0].reader().getIndex();
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

    const auto new_appended = ents.front().reader().getIndex();
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
    // Manual insertion instead of insert_range
    for (const auto& ent : ents) {
        entries_.push_back(ent.clone());
    }
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
        return snapshot_metadata_.reader().getIndex() + 1;
    }
    return entries_[0].reader().getIndex();
}

uint64_t MemoryStorageCore::last_index() const {
    if (entries_.empty()) {
        return snapshot_metadata_.reader().getIndex();
    }
    return entries_.back().reader().getIndex();
}

Snapshot MemoryStorageCore::snapshot() const {
    Snapshot snapshot;
    auto snapshot_builder = snapshot.builder();
    // Set empty data to ensure consistent serialization
    snapshot_builder.setData(::capnp::Data::Reader(nullptr, 0));
    auto meta_builder = snapshot_builder.initMetadata();

    auto hard_state_reader = raft_state_.hard_state.reader();
    meta_builder.setIndex(hard_state_reader.getCommit());

    uint64_t term;
    auto snapshot_meta_reader = snapshot_metadata_.reader();
    if (meta_builder.getIndex() < snapshot_meta_reader.getIndex()) {
        PANIC(
            "commit {} < snapshot_metadata.index {}", meta_builder.getIndex(),
            snapshot_meta_reader.getIndex()
        );
    }
    if (meta_builder.getIndex() > snapshot_meta_reader.getIndex()) {
        const uint64_t offset = entries_[0].reader().getIndex();
        term = entries_[(meta_builder.getIndex() - offset)].reader().getTerm();
    } else {
        term = snapshot_meta_reader.getTerm();
    }

    meta_builder.setTerm(term);

    // Copy conf_state
    auto conf_state_reader = raft_state_.conf_state.reader();
    auto meta_conf_builder = meta_builder.initConfState();

    auto voters = conf_state_reader.getVoters();
    auto voters_builder = meta_conf_builder.initVoters(voters.size());
    for (size_t i = 0; i < voters.size(); ++i) {
        voters_builder.set(i, voters[i]);
    }

    auto learners = conf_state_reader.getLearners();
    auto learners_builder = meta_conf_builder.initLearners(learners.size());
    for (size_t i = 0; i < learners.size(); ++i) {
        learners_builder.set(i, learners[i]);
    }

    auto voters_outgoing = conf_state_reader.getVotersOutgoing();
    auto voters_outgoing_builder = meta_conf_builder.initVotersOutgoing(voters_outgoing.size());
    for (size_t i = 0; i < voters_outgoing.size(); ++i) {
        voters_outgoing_builder.set(i, voters_outgoing[i]);
    }

    auto learners_next = conf_state_reader.getLearnersNext();
    auto learners_next_builder = meta_conf_builder.initLearnersNext(learners_next.size());
    for (size_t i = 0; i < learners_next.size(); ++i) {
        learners_next_builder.set(i, learners_next[i]);
    }

    meta_conf_builder.setAutoLeave(conf_state_reader.getAutoLeave());

    return snapshot;
}

MemoryStorage::~MemoryStorage() = default;

Result<RaftState> MemoryStorage::InitialState() {
    std::lock_guard lock(mutex_);
    return core_.raft_state_.clone();
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

    const uint64_t offset = core_.entries_.front().reader().getIndex();
    const auto lo = static_cast<int64_t>(low - offset);
    const auto hi = static_cast<int64_t>(high - offset);
    std::vector<Entry> entries;
    for (auto it = core_.entries_.begin() + lo; it != core_.entries_.begin() + hi; ++it) {
        entries.push_back(it->clone());
    }
    if (max_size) {
        LimitSize(entries, *max_size);
    }
    return entries;
}

void MemoryStorage::SetEntries(const std::vector<Entry>& entries) {
    std::lock_guard lock(mutex_);
    core_.entries_.clear();
    core_.entries_.reserve(entries.size());
    for (const auto& ent : entries) {
        core_.entries_.push_back(ent.clone());
    }
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
    core_.raft_state_ = raft_state.clone();
}

void MemoryStorage::SetConfState(const ConfState& conf_state) {
    std::lock_guard lock(mutex_);
    auto src_reader = conf_state.reader();
    core_.raft_state_.conf_state = ConfState();
    auto conf_builder = core_.raft_state_.conf_state.builder();

    // Copy voters
    auto voters = src_reader.getVoters();
    auto voters_builder = conf_builder.initVoters(voters.size());
    for (size_t i = 0; i < voters.size(); ++i) {
        voters_builder.set(i, voters[i]);
    }

    // Copy learners
    auto learners = src_reader.getLearners();
    auto learners_builder = conf_builder.initLearners(learners.size());
    for (size_t i = 0; i < learners.size(); ++i) {
        learners_builder.set(i, learners[i]);
    }

    // Copy voters_outgoing
    auto voters_outgoing = src_reader.getVotersOutgoing();
    auto voters_outgoing_builder = conf_builder.initVotersOutgoing(voters_outgoing.size());
    for (size_t i = 0; i < voters_outgoing.size(); ++i) {
        voters_outgoing_builder.set(i, voters_outgoing[i]);
    }

    // Copy learners_next
    auto learners_next = src_reader.getLearnersNext();
    auto learners_next_builder = conf_builder.initLearnersNext(learners_next.size());
    for (size_t i = 0; i < learners_next.size(); ++i) {
        learners_next_builder.set(i, learners_next[i]);
    }

    conf_builder.setAutoLeave(src_reader.getAutoLeave());
}

void MemoryStorage::TriggerSnapshotUnavailable() {
    std::lock_guard lock(mutex_);
    core_.TriggerSnapshotUnavailable();
}

void MemoryStorage::TriggerLogUnavailable(bool enable) {
    std::lock_guard lock(mutex_);
    core_.trigger_log_unavailable_ = enable;
}

std::optional<GetEntriesContext> MemoryStorage::TakeGetEntriesContext() {
    std::lock_guard lock(mutex_);
    return core_.TakeGetEntriesContext();
}

Result<void> MemoryStorage::ApplySnapshot(const Snapshot& snapshot) {
    std::lock_guard lock(mutex_);
    return core_.ApplySnapshot(snapshot);
}

std::vector<Entry> MemoryStorage::AllEntries() {
    std::lock_guard lock(mutex_);
    std::vector<Entry> result;
    result.reserve(core_.entries_.size());
    for (const auto& ent : core_.entries_) {
        result.push_back(ent.clone());
    }
    return result;
}

Result<void> MemoryStorage::MayAppend(const std::vector<Entry>& entries) {
    std::lock_guard lock(mutex_);
    return core_.MayAppend(entries);
}

Result<uint64_t> MemoryStorage::Term(const uint64_t idx) {
    std::lock_guard lock(mutex_);
    auto snapshot_meta_reader = core_.snapshot_metadata_.reader();
    if (idx == snapshot_meta_reader.getIndex()) {
        return snapshot_meta_reader.getTerm();
    }

    const auto offset = core_.first_index();
    if (idx < offset) {
        return RaftError(StorageErrorCode::Compacted);
    }

    if (idx > core_.last_index()) {
        return RaftError(StorageErrorCode::Unavailable);
    }

    return core_.entries_[idx - offset].reader().getTerm();
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
    auto meta_reader = snapshot.reader().getMetadata();
    if (meta_reader.getIndex() < request_index) {
        // Need to rebuild snapshot with new index
        Snapshot new_snapshot;
        auto builder = new_snapshot.builder();

        // Copy data from original snapshot
        auto orig_reader = snapshot.reader();
        auto orig_data = orig_reader.getData();
        builder.setData(orig_data);

        // Build metadata with updated index
        auto meta_builder = builder.initMetadata();
        meta_builder.setIndex(request_index);
        meta_builder.setTerm(meta_reader.getTerm());

        // Copy conf_state
        auto orig_conf = meta_reader.getConfState();
        auto conf_builder = meta_builder.initConfState();

        auto voters = orig_conf.getVoters();
        auto voters_builder = conf_builder.initVoters(voters.size());
        for (size_t i = 0; i < voters.size(); ++i) {
            voters_builder.set(i, voters[i]);
        }

        auto learners = orig_conf.getLearners();
        auto learners_builder = conf_builder.initLearners(learners.size());
        for (size_t i = 0; i < learners.size(); ++i) {
            learners_builder.set(i, learners[i]);
        }

        auto voters_outgoing = orig_conf.getVotersOutgoing();
        auto voters_outgoing_builder = conf_builder.initVotersOutgoing(voters_outgoing.size());
        for (size_t i = 0; i < voters_outgoing.size(); ++i) {
            voters_outgoing_builder.set(i, voters_outgoing[i]);
        }

        auto learners_next = orig_conf.getLearnersNext();
        auto learners_next_builder = conf_builder.initLearnersNext(learners_next.size());
        for (size_t i = 0; i < learners_next.size(); ++i) {
            learners_next_builder.set(i, learners_next[i]);
        }

        conf_builder.setAutoLeave(orig_conf.getAutoLeave());

        return new_snapshot;
    }
    return snapshot;
}

}  // namespace raftpp
