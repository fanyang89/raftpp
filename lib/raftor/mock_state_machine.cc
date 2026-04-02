#include "raftpp/raftor/mock_state_machine.h"

#include <array>
#include <optional>
#include <utility>

#include <nonstd/expected.hpp>
#include <nonstd/span.hpp>

#include "raftpp/core/capnp_util.h"

namespace raftpp::raftor {

Result<ApplyResult> MockStateMachine::Apply(const Entry& entry) {
    std::lock_guard lock(mutex_);
    ++apply_count_;
    if (should_fail_apply_) {
        ++apply_failure_count_;
        return nonstd::make_unexpected(RaftError(RaftErrorCode::ProposalDropped));
    }

    auto reader = capnp_util::reader<msg::Entry>(entry);
    auto data = reader.getData();
    std::string payload(reinterpret_cast<const char*>(data.begin()), data.size());
    std::string response = "OK:";
    response.append(payload);
    applied_entries_.push_back(std::move(payload));
    ++apply_success_count_;
    return ApplyResult{std::move(response)};
}

Result<SnapshotMetadata> MockStateMachine::TakeSnapshot(
    uint64_t applied_index, uint64_t applied_term, const ConfState& conf_state,
    SnapshotWriter& writer
) {
    std::lock_guard lock(mutex_);
    ++snapshot_count_;
    if (should_fail_snapshot_) {
        ++snapshot_failure_count_;
        return nonstd::make_unexpected(RaftError(StorageErrorCode::SnapshotTemporarilyUnavailable));
    }

    if (auto write_result = writer.Write(
            nonstd::span<const uint8_t>(snapshot_payload_.data(), snapshot_payload_.size())
        );
        !write_result) {
        ++snapshot_failure_count_;
        return nonstd::make_unexpected(write_result.error());
    }

    auto metadata = capnp_util::make<msg::SnapshotMetadata>();
    auto meta_builder = capnp_util::builder<msg::SnapshotMetadata>(metadata);
    meta_builder.setIndex(applied_index);
    meta_builder.setTerm(applied_term);
    meta_builder.setConfState(capnp_util::reader<msg::ConfState>(conf_state));
    last_snapshot_index_ = applied_index;
    last_snapshot_term_ = applied_term;
    ++snapshot_success_count_;
    return metadata;
}

Result<void> MockStateMachine::RestoreSnapshot(
    const SnapshotMetadata& metadata, SnapshotReader& reader
) {
    std::lock_guard lock(mutex_);
    ++restore_count_;
    if (should_fail_restore_) {
        ++restore_failure_count_;
        return nonstd::make_unexpected(RaftError(StorageErrorCode::Unavailable));
    }

    auto meta_reader = capnp_util::reader<msg::SnapshotMetadata>(metadata);
    last_restored_index_ = meta_reader.getIndex();
    last_restored_term_ = meta_reader.getTerm();
    last_restored_data_.clear();

    std::array<uint8_t, 4096> buffer{};
    while (true) {
        auto read_result = reader.Read(buffer);
        if (!read_result) {
            ++restore_failure_count_;
            return nonstd::make_unexpected(read_result.error());
        }
        const size_t bytes_read = *read_result;
        if (bytes_read == 0) {
            break;
        }
        last_restored_data_.insert(
            last_restored_data_.end(), buffer.begin(), buffer.begin() + bytes_read
        );
    }

    ++restore_success_count_;
    return {};
}

void MockStateMachine::OnLeadershipChange(bool is_leader, uint64_t term, uint64_t leader_id) {
    std::lock_guard lock(mutex_);
    is_leader_ = is_leader;
    current_term_ = term;
    current_leader_ = leader_id;
    ++leadership_change_count_;
}

void MockStateMachine::OnPeerUnreachable(uint64_t peer_id) {
    std::lock_guard lock(mutex_);
    unreachable_peers_.push_back(peer_id);
}

void MockStateMachine::SetShouldFailApply(bool fail) {
    std::lock_guard lock(mutex_);
    should_fail_apply_ = fail;
}

void MockStateMachine::SetShouldFailSnapshot(bool fail) {
    std::lock_guard lock(mutex_);
    should_fail_snapshot_ = fail;
}

void MockStateMachine::SetShouldFailRestore(bool fail) {
    std::lock_guard lock(mutex_);
    should_fail_restore_ = fail;
}

void MockStateMachine::SetSnapshotData(std::vector<uint8_t> data) {
    std::lock_guard lock(mutex_);
    snapshot_payload_ = std::move(data);
}

void MockStateMachine::Reset() {
    std::lock_guard lock(mutex_);
    apply_count_ = 0;
    snapshot_count_ = 0;
    restore_count_ = 0;
    leadership_change_count_ = 0;
    apply_success_count_ = 0;
    apply_failure_count_ = 0;
    snapshot_success_count_ = 0;
    snapshot_failure_count_ = 0;
    restore_success_count_ = 0;
    restore_failure_count_ = 0;
    is_leader_ = false;
    current_term_ = 0;
    current_leader_ = 0;
    last_snapshot_index_ = 0;
    last_snapshot_term_ = 0;
    last_restored_index_ = 0;
    last_restored_term_ = 0;
    applied_entries_.clear();
    unreachable_peers_.clear();
    last_restored_data_.clear();
}

size_t MockStateMachine::ApplyCount() const {
    std::lock_guard lock(mutex_);
    return apply_count_;
}

size_t MockStateMachine::SnapshotCount() const {
    std::lock_guard lock(mutex_);
    return snapshot_count_;
}

size_t MockStateMachine::RestoreCount() const {
    std::lock_guard lock(mutex_);
    return restore_count_;
}

size_t MockStateMachine::LeadershipChangeCount() const {
    std::lock_guard lock(mutex_);
    return leadership_change_count_;
}

size_t MockStateMachine::ApplySuccessCount() const {
    std::lock_guard lock(mutex_);
    return apply_success_count_;
}

size_t MockStateMachine::ApplyFailureCount() const {
    std::lock_guard lock(mutex_);
    return apply_failure_count_;
}

size_t MockStateMachine::SnapshotSuccessCount() const {
    std::lock_guard lock(mutex_);
    return snapshot_success_count_;
}

size_t MockStateMachine::SnapshotFailureCount() const {
    std::lock_guard lock(mutex_);
    return snapshot_failure_count_;
}

size_t MockStateMachine::RestoreSuccessCount() const {
    std::lock_guard lock(mutex_);
    return restore_success_count_;
}

size_t MockStateMachine::RestoreFailureCount() const {
    std::lock_guard lock(mutex_);
    return restore_failure_count_;
}

bool MockStateMachine::IsLeader() const {
    std::lock_guard lock(mutex_);
    return is_leader_;
}

uint64_t MockStateMachine::CurrentTerm() const {
    std::lock_guard lock(mutex_);
    return current_term_;
}

uint64_t MockStateMachine::CurrentLeader() const {
    std::lock_guard lock(mutex_);
    return current_leader_;
}

uint64_t MockStateMachine::LastSnapshotIndex() const {
    std::lock_guard lock(mutex_);
    return last_snapshot_index_;
}

uint64_t MockStateMachine::LastSnapshotTerm() const {
    std::lock_guard lock(mutex_);
    return last_snapshot_term_;
}

uint64_t MockStateMachine::LastRestoredIndex() const {
    std::lock_guard lock(mutex_);
    return last_restored_index_;
}

uint64_t MockStateMachine::LastRestoredTerm() const {
    std::lock_guard lock(mutex_);
    return last_restored_term_;
}

std::vector<std::string> MockStateMachine::AppliedEntries() const {
    std::lock_guard lock(mutex_);
    return applied_entries_;
}

std::vector<uint64_t> MockStateMachine::UnreachablePeers() const {
    std::lock_guard lock(mutex_);
    return unreachable_peers_;
}

std::vector<uint8_t> MockStateMachine::LastRestoredData() const {
    std::lock_guard lock(mutex_);
    return last_restored_data_;
}

std::vector<uint8_t> MockStateMachine::SnapshotPayload() const {
    std::lock_guard lock(mutex_);
    return snapshot_payload_;
}

}  // namespace raftpp::raftor
