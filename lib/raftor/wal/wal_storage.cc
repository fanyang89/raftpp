#include "raftpp/raftor/wal/wal_storage.h"

#include <spdlog/spdlog.h>

namespace raftpp::raftor::wal {

WALStorage::~WALStorage() = default;

Result<std::shared_ptr<WALStorage>> WALStorage::Open(const WALConfig& config) {
    auto storage = std::shared_ptr<WALStorage>(new WALStorage());

    auto wal = WAL::Open(config);
    if (!wal) {
        return wal.error();
    }

    storage->wal_ = std::move(*wal);

    return storage;
}

Result<RaftState> WALStorage::InitialState() {
    std::lock_guard lock(mutex_);

    RaftState state;
    state.hard_state = wal_->GetHardState().clone();
    state.conf_state = wal_->GetConfState().clone();

    return state;
}

Result<std::vector<Entry>> WALStorage::Entries(
    uint64_t low, uint64_t high, std::optional<uint64_t> max_size, GetEntriesContext /*context*/
) {
    std::lock_guard lock(mutex_);

    return wal_->ReadEntries(low, high, max_size);
}

Result<uint64_t> WALStorage::Term(uint64_t idx) {
    std::lock_guard lock(mutex_);

    // Check snapshot first
    auto snap_reader = snapshot_.reader();
    auto snap_meta = snap_reader.getMetadata();
    if (idx == snap_meta.getIndex() && snap_meta.getIndex() > 0) {
        return snap_meta.getTerm();
    }

    return wal_->Term(idx);
}

Result<uint64_t> WALStorage::FirstIndex() {
    std::lock_guard lock(mutex_);

    return wal_->FirstIndex();
}

Result<uint64_t> WALStorage::LastIndex() {
    std::lock_guard lock(mutex_);

    return wal_->LastIndex();
}

Result<Snapshot> WALStorage::GetSnapshot(uint64_t /*request_index*/, uint64_t /*to*/) {
    std::lock_guard lock(mutex_);

    auto snap_meta = snapshot_.reader().getMetadata();
    if (snap_meta.getIndex() == 0) {
        return RaftError(StorageErrorCode::SnapshotTemporarilyUnavailable);
    }

    return snapshot_.clone();
}

void WALStorage::SetHardState(HardState&& hs) {
    std::lock_guard lock(mutex_);

    auto result = wal_->SaveHardState(hs);
    if (!result) {
        SPDLOG_ERROR("failed to save hard state: {}", result.error().ToString());
    }
}

Result<void> WALStorage::Append(const std::vector<Entry>& entries) {
    std::lock_guard lock(mutex_);

    return wal_->Append(entries);
}

Result<void> WALStorage::Compact(uint64_t compact_index) {
    std::lock_guard lock(mutex_);

    return wal_->Compact(compact_index);
}

Result<void> WALStorage::ApplySnapshot(const Snapshot& snapshot) {
    std::lock_guard lock(mutex_);

    // Store snapshot in memory
    snapshot_ = snapshot.clone();

    // Apply to WAL
    return wal_->ApplySnapshot(snapshot);
}

void WALStorage::SetConfState(const ConfState& conf_state) {
    std::lock_guard lock(mutex_);

    // Update the conf state in the hard state
    HardState hs = wal_->GetHardState().clone();
    auto result = wal_->SaveHardState(hs);
    if (!result) {
        SPDLOG_ERROR("failed to save conf state: {}", result.error().ToString());
    }
}

Result<void> WALStorage::Sync() {
    std::lock_guard lock(mutex_);

    return wal_->Sync();
}

std::vector<Entry> WALStorage::AllEntries() {
    std::lock_guard lock(mutex_);

    uint64_t first = wal_->FirstIndex();
    uint64_t last = wal_->LastIndex();

    if (first > last) {
        return {};
    }

    auto result = wal_->ReadEntries(first, last + 1, std::nullopt);
    if (!result) {
        SPDLOG_ERROR("failed to read all entries: {}", result.error().ToString());
        return {};
    }

    return std::move(*result);
}

}  // namespace raftpp::raftor::wal
