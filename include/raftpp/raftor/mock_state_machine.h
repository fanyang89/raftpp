#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "raftpp/core/error.h"
#include "raftpp/core/types.h"
#include "raftpp/raftor/state_machine.h"

namespace raftpp::raftor {

/// A simple in-memory StateMachine implementation for integration tests and wiring.
class MockStateMachine final : public StateMachine {
  public:
    Result<ApplyResult> Apply(const Entry& entry) override;
    Result<SnapshotMetadata> TakeSnapshot(
        uint64_t applied_index, uint64_t applied_term, const ConfState& conf_state,
        SnapshotWriter& writer
    ) override;
    Result<void> RestoreSnapshot(const SnapshotMetadata& metadata, SnapshotReader& reader) override;
    void OnLeadershipChange(bool is_leader, uint64_t term, uint64_t leader_id) override;
    void OnPeerUnreachable(uint64_t peer_id) override;

    // Configuration
    void SetShouldFailApply(bool fail);
    void SetShouldFailSnapshot(bool fail);
    void SetShouldFailRestore(bool fail);
    void SetSnapshotData(std::vector<uint8_t> data);

    // Clears counters and observed state while keeping failure configuration and snapshot payload.
    void Reset();

    // Observability helpers
    size_t ApplyCount() const;
    size_t SnapshotCount() const;
    size_t RestoreCount() const;
    size_t LeadershipChangeCount() const;
    size_t ApplySuccessCount() const;
    size_t ApplyFailureCount() const;
    size_t SnapshotSuccessCount() const;
    size_t SnapshotFailureCount() const;
    size_t RestoreSuccessCount() const;
    size_t RestoreFailureCount() const;
    bool IsLeader() const;
    uint64_t CurrentTerm() const;
    uint64_t CurrentLeader() const;
    uint64_t LastSnapshotIndex() const;
    uint64_t LastSnapshotTerm() const;
    uint64_t LastRestoredIndex() const;
    uint64_t LastRestoredTerm() const;
    std::vector<std::string> AppliedEntries() const;
    std::vector<uint64_t> UnreachablePeers() const;
    std::vector<uint8_t> LastRestoredData() const;
    std::vector<uint8_t> SnapshotPayload() const;

  private:
    mutable std::mutex mutex_;
    size_t apply_count_ = 0;
    size_t snapshot_count_ = 0;
    size_t restore_count_ = 0;
    size_t leadership_change_count_ = 0;
    size_t apply_success_count_ = 0;
    size_t apply_failure_count_ = 0;
    size_t snapshot_success_count_ = 0;
    size_t snapshot_failure_count_ = 0;
    size_t restore_success_count_ = 0;
    size_t restore_failure_count_ = 0;
    bool is_leader_ = false;
    uint64_t current_term_ = 0;
    uint64_t current_leader_ = 0;
    uint64_t last_snapshot_index_ = 0;
    uint64_t last_snapshot_term_ = 0;
    uint64_t last_restored_index_ = 0;
    uint64_t last_restored_term_ = 0;
    bool should_fail_apply_ = false;
    bool should_fail_snapshot_ = false;
    bool should_fail_restore_ = false;
    std::vector<std::string> applied_entries_;
    std::vector<uint64_t> unreachable_peers_;
    std::vector<uint8_t> snapshot_payload_ = {'s', 'n', 'a', 'p'};
    std::vector<uint8_t> last_restored_data_;
};

}  // namespace raftpp::raftor
