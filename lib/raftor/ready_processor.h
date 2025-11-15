#pragma once

#include <memory>

#include "raftpp/core/error.h"
#include "raftpp/core/raft_core.h"
#include "raftpp/core/raw_node.h"
#include "raftpp/core/storage.h"
#include "raftpp/raftor/proposal_tracker.h"
#include "raftpp/raftor/rpc/transport.h"
#include "raftpp/raftor/state_machine.h"
#include "raftpp/raftor/wal/wal_storage.h"

namespace raftpp::raftor {

/// Handles Ready processing in the correct order
///
/// The Ready processing order is critical for correctness:
/// 1. Persist entries to stable storage (WAL)
/// 2. Persist hard state (term, vote, commit)
/// 3. Apply snapshot (if present)
/// 4. Send messages to peers
/// 5. Apply committed entries to state machine
/// 6. Advance the Raft state
class ReadyProcessor {
  public:
    ReadyProcessor(
        RawNode& raw_node, std::shared_ptr<wal::WALStorage> storage, StateMachine& state_machine,
        rpc::Transport& transport, ProposalTracker& proposal_tracker
    );

    /// Process one Ready cycle
    ///
    /// @return true if there was work to do
    [[nodiscard]] Result<bool> Process();

    /// Get the current leadership state for notifications
    [[nodiscard]] bool IsLeader() const { return prev_role_ == StateRole::Leader; }

    [[nodiscard]] uint64_t GetLeaderId() const { return prev_leader_; }

    [[nodiscard]] uint64_t GetAppliedIndex() const { return applied_index_; }

  private:
    /// Persist entries from Ready to WAL
    [[nodiscard]] Result<void> PersistEntries(const Ready& rd);

    /// Persist hard state from Ready
    [[nodiscard]] Result<void> PersistHardState(const Ready& rd);

    /// Apply snapshot from Ready (if present)
    [[nodiscard]] Result<void> ApplySnapshot(const Ready& rd);

    /// Send messages to peers
    void SendMessages(const std::vector<Message>& messages);

    /// Apply committed entries to state machine
    [[nodiscard]] Result<void> ApplyCommittedEntries(const std::vector<Entry>& entries);

    /// Process a single committed entry
    [[nodiscard]] Result<void> ApplyEntry(const Entry& entry);

    /// Process light ready (after Advance)
    void ProcessLightReady(const LightReady& light_rd);

    /// Check for leadership changes and notify state machine
    void CheckLeadershipChange(const Ready& rd);

    /// Convert Cap'n Proto Snapshot to SnapshotData
    [[nodiscard]] static SnapshotData ToSnapshotData(const Snapshot& snapshot);

    RawNode& raw_node_;
    std::shared_ptr<wal::WALStorage> storage_;
    StateMachine& state_machine_;
    rpc::Transport& transport_;
    ProposalTracker& proposal_tracker_;

    // Track leadership changes
    StateRole prev_role_ = StateRole::Follower;
    uint64_t prev_leader_ = 0;
    uint64_t prev_term_ = 0;

    // Track applied index
    uint64_t applied_index_ = 0;
};

}  // namespace raftpp::raftor
