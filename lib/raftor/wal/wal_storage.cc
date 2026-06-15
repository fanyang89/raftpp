#include "raftpp/raftor/wal/wal_storage.h"

#include <optional>
#include <utility>

#include <nonstd/span.hpp>
#include <opentelemetry/trace/span.h>

#include "raftpp/core/capnp_util.h"
#include "raftpp/logging.h"
#include "raftpp/raftor/telemetry.h"
#include "raftpp/raftor/wal/wal.h"

namespace raftpp::raftor::wal {

WALStorage::WALStorage()
    : effective_io_backend_(WALIoBackend::Auto),
      io_backend_note_(),
      snapshot_(capnp_util::make<msg::Snapshot>()) {}

WALStorage::~WALStorage() = default;

Result<std::shared_ptr<WALStorage>> WALStorage::Open(const WALConfig& config) {
    auto storage = std::shared_ptr<WALStorage>(new WALStorage());

    auto wal = WAL::Open(config);
    if (!wal) {
        return wal.error();
    }

    storage->wal_ = std::move(*wal);

    if (storage->wal_->SnapshotIndex() > 0) {
        auto snapshot = storage->wal_->LoadSnapshot();
        if (!snapshot) {
            return snapshot.error();
        }
        storage->snapshot_ = std::move(*snapshot);
    }

    {
        std::lock_guard lock(storage->mutex_);
        storage->effective_io_backend_ = storage->wal_->EffectiveIoBackend();
        storage->io_backend_note_ = std::string(storage->wal_->IoBackendNote());
    }

    return storage;
}

WALIoBackend WALStorage::EffectiveIoBackend() const {
    std::lock_guard lock(mutex_);
    return effective_io_backend_;
}

std::string_view WALStorage::IoBackendNote() const {
    std::lock_guard lock(mutex_);
    return io_backend_note_;
}

Result<RaftState> WALStorage::InitialState() {
    std::lock_guard lock(mutex_);

    RaftState state;
    state.hard_state = CloneHardState(wal_->GetHardState());
    state.conf_state = CloneConfState(wal_->GetConfState());

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
    auto snap_reader = capnp_util::reader<msg::Snapshot>(snapshot_);
    auto snap_meta = snap_reader.getMetadata();
    if (idx == snap_meta.getIndex() && snap_meta.getIndex() > 0) {
        return snap_meta.getTerm();
    }

    // Special case: index 0 before any entries or snapshot
    if (idx == 0 && snap_meta.getIndex() == 0) {
        return 0;
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

Result<Snapshot> WALStorage::GetSnapshot(uint64_t request_index, uint64_t /*to*/) {
    std::lock_guard lock(mutex_);

    auto snap_meta = capnp_util::reader<msg::Snapshot>(snapshot_).getMetadata();
    if (snap_meta.getIndex() == 0 || snap_meta.getIndex() < request_index) {
        return RaftError(StorageErrorCode::SnapshotTemporarilyUnavailable);
    }

    return CloneSnapshot(snapshot_);
}

Result<void> WALStorage::SetHardState(HardState&& hs) {
    std::lock_guard lock(mutex_);

    auto result = wal_->SaveHardState(std::move(hs));
    if (!result) {
        RAFTPP_LOG_ERROR("failed to save hard state: {}", result.error().ToString());
        return result.error();
    }
    return {};
}

Result<void> WALStorage::Append(const std::vector<Entry>& entries) {
    std::lock_guard lock(mutex_);

    telemetry::ScopedSpan span("raftor.wal.append");
    span.span()->SetAttribute("raft.entry.count", static_cast<int64_t>(entries.size()));

    auto result = wal_->Append(entries);
    telemetry::RecordErrorIf(span.span(), result);
    return result;
}

Result<void> WALStorage::Compact(uint64_t compact_index) {
    std::lock_guard lock(mutex_);

    return wal_->Compact(compact_index);
}

Result<void> WALStorage::ApplySnapshot(const Snapshot& snapshot) {
    std::lock_guard lock(mutex_);

    telemetry::ScopedSpan span("raftor.wal.apply_snapshot");
    auto snap_reader = capnp_util::reader<msg::Snapshot>(snapshot);
    auto snap_meta = snap_reader.getMetadata();
    span.span()->SetAttribute("raft.snapshot.index", static_cast<int64_t>(snap_meta.getIndex()));
    span.span()->SetAttribute("raft.snapshot.term", static_cast<int64_t>(snap_meta.getTerm()));

    // Apply to WAL first so the in-memory cache only advances after durable persistence succeeds.
    auto result = wal_->ApplySnapshot(snapshot);
    if (result) {
        snapshot_ = CloneSnapshot(snapshot);
    }
    telemetry::RecordErrorIf(span.span(), result);
    return result;
}

Result<void> WALStorage::ApplyLocalSnapshot(const Snapshot& snapshot) {
    std::lock_guard lock(mutex_);

    telemetry::ScopedSpan span("raftor.wal.apply_local_snapshot");
    auto snap_reader = capnp_util::reader<msg::Snapshot>(snapshot);
    auto snap_meta = snap_reader.getMetadata();
    span.span()->SetAttribute("raft.snapshot.index", static_cast<int64_t>(snap_meta.getIndex()));
    span.span()->SetAttribute("raft.snapshot.term", static_cast<int64_t>(snap_meta.getTerm()));

    auto result = wal_->ApplyLocalSnapshot(snapshot);
    if (result) {
        snapshot_ = CloneSnapshot(snapshot);
    }
    telemetry::RecordErrorIf(span.span(), result);
    return result;
}

Result<void> WALStorage::SetConfState(const ConfState& conf_state) {
    std::lock_guard lock(mutex_);

    auto result = wal_->SaveConfState(conf_state);
    if (!result) {
        RAFTPP_LOG_ERROR("failed to save conf state: {}", result.error().ToString());
        return result.error();
    }
    return {};
}

std::vector<PeerAddress> WALStorage::GetPeerAddresses() const {
    std::lock_guard lock(mutex_);
    return wal_->GetPeerAddresses();
}

Result<void> WALStorage::SetPeerAddresses(std::vector<PeerAddress> peer_addresses) {
    std::lock_guard lock(mutex_);
    return wal_->SavePeerAddresses(std::move(peer_addresses));
}

Result<void> WALStorage::UpsertPeerAddress(uint64_t id, std::string addr) {
    std::lock_guard lock(mutex_);
    return wal_->UpsertPeerAddress(id, std::move(addr));
}

Result<void> WALStorage::RemovePeerAddress(uint64_t id) {
    std::lock_guard lock(mutex_);
    return wal_->RemovePeerAddress(id);
}

Result<void> WALStorage::Sync() {
    std::lock_guard lock(mutex_);

    telemetry::ScopedSpan span("raftor.wal.sync");
    auto result = wal_->Sync();
    telemetry::RecordErrorIf(span.span(), result);
    return result;
}

Result<std::optional<Snapshot>> WALStorage::LocalSnapshot() {
    std::lock_guard lock(mutex_);

    auto snap_meta = capnp_util::reader<msg::Snapshot>(snapshot_).getMetadata();
    if (snap_meta.getIndex() == 0) {
        return std::nullopt;
    }

    return std::optional<Snapshot>{CloneSnapshot(snapshot_)};
}

uint64_t WALStorage::LogSizeBytes() const {
    std::lock_guard lock(mutex_);
    if (!wal_) {
        return 0;
    }
    return wal_->LogSizeBytes();
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
        RAFTPP_LOG_ERROR("failed to read all entries: {}", result.error().ToString());
        return {};
    }

    return std::move(*result);
}

uint64_t WALStorage::SnapshotIndex() const {
    std::lock_guard lock(mutex_);
    if (!wal_) {
        return 0;
    }
    return wal_->SnapshotIndex();
}

bool WALStorage::IsInitialized() const {
    std::lock_guard lock(mutex_);
    auto conf_reader = capnp_util::reader<msg::ConfState>(wal_->GetConfState());
    return conf_reader.getVoters().size() > 0;
}

}  // namespace raftpp::raftor::wal
