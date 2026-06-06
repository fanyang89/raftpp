#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

#include <capnp/blob.h>
#include <nonstd/span.hpp>

#include "raftpp/core/error.h"
#include "raftpp/core/types.h"

namespace raftpp::raftor {

/// Result of applying an entry to the state machine
struct ApplyResult {
    /// Optional response data to return to the proposer
    std::optional<std::string> response;
};

/// Streaming sink for snapshot bytes.
class SnapshotWriter {
  public:
    virtual ~SnapshotWriter() = default;
    [[nodiscard]] virtual Result<void> Write(nonstd::span<const uint8_t> chunk) = 0;
};

/// Streaming source for snapshot bytes.
class SnapshotReader {
  public:
    virtual ~SnapshotReader() = default;
    /// Read up to out.size() bytes. Returns 0 on EOF.
    [[nodiscard]] virtual Result<size_t> Read(nonstd::span<uint8_t> out) = 0;
};

/// SnapshotReader implementation backed by an in-memory Cap'n Proto Data field.
class SnapshotDataReader final : public SnapshotReader {
  public:
    explicit SnapshotDataReader(::capnp::Data::Reader data) : data_(data) {}

    Result<size_t> Read(nonstd::span<uint8_t> out) override {
        if (offset_ >= data_.size() || out.empty()) {
            return 0;
        }

        const size_t remaining = data_.size() - offset_;
        const size_t bytes_to_copy = std::min(out.size(), remaining);
        std::memcpy(out.data(), data_.begin() + offset_, bytes_to_copy);
        offset_ += bytes_to_copy;
        return bytes_to_copy;
    }

  private:
    ::capnp::Data::Reader data_;
    size_t offset_ = 0;
};

/// The StateMachine interface that users must implement
///
/// This interface provides the application-specific logic for:
/// - Applying committed log entries
/// - Creating and restoring snapshots
/// - Handling leadership transitions
class StateMachine {
  public:
    virtual ~StateMachine() = default;

    /// Apply a committed entry to the state machine
    ///
    /// This is called for each entry in commit order. The entry may be:
    /// - A normal data entry (EntryNormal)
    /// - A configuration change entry (EntryConfChange, EntryConfChangeV2)
    /// - An empty entry (after leader election)
    ///
    /// For conf change entries, Raftor handles the conf change application,
    /// but the state machine is notified for any side effects.
    ///
    /// @param entry The committed entry to apply
    /// @return ApplyResult with optional response, or error
    [[nodiscard]] virtual Result<ApplyResult> Apply(const Entry& entry) = 0;

    /// Create a snapshot of the current state machine state
    ///
    /// Called when compaction is needed or when a follower is too far behind.
    /// The snapshot should include all state up to and including `applied_index`.
    ///
    /// @param applied_index The last applied index to include
    /// @param applied_term The term of the last applied entry
    /// @param conf_state The current cluster configuration
    /// @param writer Streaming sink for snapshot payload bytes
    /// @return Snapshot metadata (index, term, conf_state), or error
    [[nodiscard]] virtual Result<SnapshotMetadata> TakeSnapshot(
        uint64_t applied_index, uint64_t applied_term, const ConfState& conf_state,
        SnapshotWriter& writer
    ) = 0;

    /// Restore state machine from a snapshot
    ///
    /// Called when this node receives a snapshot from the leader.
    /// The state machine should completely replace its state with the snapshot.
    ///
    /// @param metadata The snapshot metadata (index, term, conf_state)
    /// @param reader Streaming source of snapshot payload bytes
    /// @return void on success, or error
    [[nodiscard]] virtual Result<void> RestoreSnapshot(
        const SnapshotMetadata& metadata, SnapshotReader& reader
    ) = 0;

    /// Called when leadership status changes
    ///
    /// This is informational - the state machine can use this to:
    /// - Start/stop leader-only operations
    /// - Update metrics
    /// - Notify external systems
    ///
    /// Default implementation does nothing.
    ///
    /// @param is_leader Whether this node is now the leader
    /// @param term The current term
    /// @param leader_id The leader's ID (0 if unknown)
    virtual void OnLeadershipChange(bool is_leader, uint64_t term, uint64_t leader_id) {
        (void)is_leader;
        (void)term;
        (void)leader_id;
    }

    /// Called when a peer becomes unreachable
    ///
    /// Default implementation does nothing.
    ///
    /// @param peer_id The ID of the unreachable peer
    virtual void OnPeerUnreachable(uint64_t peer_id) { (void)peer_id; }
};

}  // namespace raftpp::raftor
