#pragma once

#include <stdint.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "raftpp/core/error.h"
#include "raftpp/core/raft_core.h"
#include "raftpp/core/types.h"

namespace raftpp::raftor {

class ProposalTracker;
class StateMachine;

namespace rpc {
class Transport;
}  // namespace rpc

namespace wal {
class WALStorage;
}  // namespace wal

}  // namespace raftpp::raftor

namespace raftpp {
class RawNode;
struct LightReady;
struct ReadState;
struct Ready;
}  // namespace raftpp

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
        rpc::Transport& transport, ProposalTracker& proposal_tracker, uint64_t node_id,
        bool checksum_enabled, uint64_t initial_applied_index = 0
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

    /// Validate new entries before persisting or forwarding them.
    [[nodiscard]] Result<void> ValidateReadyEntries(const std::vector<Entry>& entries);

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
    [[nodiscard]] Result<void> ProcessLightReady(const LightReady& light_rd);

    void EnterFatalState(const RaftError& error);

    /// Record read states from Ready
    void EnqueueReadStates(const std::vector<ReadState>& read_states);

    /// Complete pending reads whose index has been applied
    void MaybeCompletePendingReads();

    /// Check for leadership changes and notify state machine
    void CheckLeadershipChange(const Ready& rd);

    RawNode& raw_node_;
    std::shared_ptr<wal::WALStorage> storage_;
    StateMachine& state_machine_;
    rpc::Transport& transport_;
    ProposalTracker& proposal_tracker_;
    uint64_t node_id_ = 0;

    // Track leadership changes
    StateRole prev_role_ = StateRole::Follower;
    uint64_t prev_leader_ = 0;
    uint64_t prev_term_ = 0;

    // Track applied index
    uint64_t applied_index_ = 0;
    std::optional<RaftError> fatal_error_;
    bool checksum_enabled_ = false;

    struct PendingRead {
        uint64_t index = 0;
        std::string ctx;
    };

    std::vector<PendingRead> pending_reads_;
};

}  // namespace raftpp::raftor
