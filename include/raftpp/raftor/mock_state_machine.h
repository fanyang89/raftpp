#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "raftpp/core/error.h"
#include "raftpp/core/types.h"
#include "raftpp/raftor/state_machine.h"

namespace raftpp::raftor {

/// A simple in-memory StateMachine implementation for integration tests and wiring.
class MockStateMachine final : public StateMachine {
  public:
    Result<ApplyResult> Apply(const Entry& entry) override {
        std::lock_guard lock(mutex_);
        ++apply_count_;
        if (should_fail_apply_) {
            return std::unexpected(RaftError(RaftErrorCode::ProposalDropped));
        }
        auto reader = capnp_util::reader<msg::Entry>(entry);
        auto data = reader.getData();
        applied_entries_.emplace_back(reinterpret_cast<const char*>(data.begin()), data.size());
        return ApplyResult{.response = "OK:" + applied_entries_.back()};
    }

    Result<SnapshotData> TakeSnapshot(
        uint64_t applied_index, uint64_t applied_term, const ConfState& conf_state
    ) override {
        std::lock_guard lock(mutex_);
        ++snapshot_count_;
        if (should_fail_snapshot_) {
            return std::unexpected(RaftError(StorageErrorCode::SnapshotTemporarilyUnavailable));
        }
        SnapshotData data;
        data.data = snapshot_payload_;
        data.metadata = capnp_util::make<msg::SnapshotMetadata>();
        auto meta_builder = capnp_util::builder<msg::SnapshotMetadata>(data.metadata);
        meta_builder.setIndex(applied_index);
        meta_builder.setTerm(applied_term);
        meta_builder.setConfState(capnp_util::reader<msg::ConfState>(conf_state));
        last_snapshot_index_ = applied_index;
        last_snapshot_term_ = applied_term;
        return data;
    }

    Result<void> RestoreSnapshot(const SnapshotData& snapshot) override {
        std::lock_guard lock(mutex_);
        ++restore_count_;
        if (should_fail_restore_) {
            return std::unexpected(RaftError(StorageErrorCode::Unavailable));
        }
        auto meta_reader = capnp_util::reader<msg::SnapshotMetadata>(snapshot.metadata);
        last_restored_index_ = meta_reader.getIndex();
        last_restored_term_ = meta_reader.getTerm();
        last_restored_data_ = snapshot.data;
        return {};
    }

    void OnLeadershipChange(bool is_leader, uint64_t term, uint64_t leader_id) override {
        std::lock_guard lock(mutex_);
        is_leader_ = is_leader;
        current_term_ = term;
        current_leader_ = leader_id;
        ++leadership_change_count_;
    }

    void OnPeerUnreachable(uint64_t peer_id) override {
        std::lock_guard lock(mutex_);
        unreachable_peers_.push_back(peer_id);
    }

    // Configuration
    void SetShouldFailApply(bool fail) {
        std::lock_guard lock(mutex_);
        should_fail_apply_ = fail;
    }

    void SetShouldFailSnapshot(bool fail) {
        std::lock_guard lock(mutex_);
        should_fail_snapshot_ = fail;
    }

    void SetShouldFailRestore(bool fail) {
        std::lock_guard lock(mutex_);
        should_fail_restore_ = fail;
    }

    void SetSnapshotData(std::vector<uint8_t> data) {
        std::lock_guard lock(mutex_);
        snapshot_payload_ = std::move(data);
    }

    // Observability helpers
    size_t ApplyCount() const {
        std::lock_guard lock(mutex_);
        return apply_count_;
    }

    size_t SnapshotCount() const {
        std::lock_guard lock(mutex_);
        return snapshot_count_;
    }

    size_t RestoreCount() const {
        std::lock_guard lock(mutex_);
        return restore_count_;
    }

    size_t LeadershipChangeCount() const {
        std::lock_guard lock(mutex_);
        return leadership_change_count_;
    }

    bool IsLeader() const {
        std::lock_guard lock(mutex_);
        return is_leader_;
    }

    uint64_t CurrentTerm() const {
        std::lock_guard lock(mutex_);
        return current_term_;
    }

    uint64_t CurrentLeader() const {
        std::lock_guard lock(mutex_);
        return current_leader_;
    }

    uint64_t LastSnapshotIndex() const {
        std::lock_guard lock(mutex_);
        return last_snapshot_index_;
    }

    uint64_t LastSnapshotTerm() const {
        std::lock_guard lock(mutex_);
        return last_snapshot_term_;
    }

    uint64_t LastRestoredIndex() const {
        std::lock_guard lock(mutex_);
        return last_restored_index_;
    }

    uint64_t LastRestoredTerm() const {
        std::lock_guard lock(mutex_);
        return last_restored_term_;
    }

    std::vector<std::string> AppliedEntries() const {
        std::lock_guard lock(mutex_);
        return applied_entries_;
    }

    std::vector<uint64_t> UnreachablePeers() const {
        std::lock_guard lock(mutex_);
        return unreachable_peers_;
    }

    std::vector<uint8_t> LastRestoredData() const {
        std::lock_guard lock(mutex_);
        return last_restored_data_;
    }

  private:
    mutable std::mutex mutex_;
    size_t apply_count_ = 0;
    size_t snapshot_count_ = 0;
    size_t restore_count_ = 0;
    size_t leadership_change_count_ = 0;
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
