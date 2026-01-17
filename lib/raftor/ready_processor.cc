#include "ready_processor.h"

#include <spdlog/spdlog.h>

namespace raftpp::raftor {

ReadyProcessor::ReadyProcessor(
    RawNode& raw_node, std::shared_ptr<wal::WALStorage> storage, StateMachine& state_machine,
    rpc::Transport& transport, ProposalTracker& proposal_tracker
)
    : raw_node_(raw_node),
      storage_(std::move(storage)),
      state_machine_(state_machine),
      transport_(transport),
      proposal_tracker_(proposal_tracker) {}

Result<bool> ReadyProcessor::Process() {
    if (!raw_node_.HasReady()) {
        return false;
    }

    Ready rd = raw_node_.GetReady();

    // Check for leadership changes before processing
    CheckLeadershipChange(rd);

    // 1. Persist entries to WAL
    if (auto result = PersistEntries(rd); !result) {
        return result.error();
    }

    // 2. Persist hard state
    if (auto result = PersistHardState(rd); !result) {
        return result.error();
    }

    // 3. Apply snapshot if present
    if (auto result = ApplySnapshot(rd); !result) {
        return result.error();
    }

    // 4. Send messages
    SendMessages(rd.Messages());

    // 5. Apply committed entries to state machine
    if (auto result = ApplyCommittedEntries(rd.light.committed_entries); !result) {
        return result.error();
    }

    // 6. Process read states
    for (const auto& rs : rd.read_states) {
        proposal_tracker_.CompleteRead(rs.request_ctx);
    }

    // 7. Advance and get light ready
    LightReady light_rd = raw_node_.Advance(rd);

    // 8. Process light ready
    ProcessLightReady(light_rd);

    return true;
}

Result<void> ReadyProcessor::PersistEntries(const Ready& rd) {
    if (rd.entries.empty()) {
        return {};
    }

    if (auto result = storage_->Append(rd.entries); !result) {
        spdlog::error("Failed to persist entries: {}", result.error().ToString());
        return std::unexpected(RaftError(RaftErrorCode::ProposalDropped));
    }

    return {};
}

Result<void> ReadyProcessor::PersistHardState(const Ready& rd) {
    if (!rd.hs) {
        return {};
    }

    // SetHardState doesn't return error in current implementation
    storage_->SetHardState(HardState{*rd.hs});

    // Sync if required
    if (rd.must_sync) {
        if (auto result = storage_->Sync(); !result) {
            spdlog::error("Failed to sync storage: {}", result.error().ToString());
            return std::unexpected(RaftError(RaftErrorCode::ProposalDropped));
        }
    }

    return {};
}

Result<void> ReadyProcessor::ApplySnapshot(const Ready& rd) {
    const auto& snapshot = rd.snapshot;
    if (snapshot.metadata().index() == 0) {
        return {};  // No snapshot
    }

    spdlog::info(
        "Applying snapshot at index {} term {}", snapshot.metadata().index(),
        snapshot.metadata().term()
    );

    // First restore to state machine
    auto snapshot_data = ToSnapshotData(snapshot);
    if (auto result = state_machine_.RestoreSnapshot(snapshot_data); !result) {
        spdlog::error("Failed to restore snapshot to state machine");
        return result.error();
    }

    // Then apply to storage
    if (auto result = storage_->ApplySnapshot(snapshot); !result) {
        spdlog::error("Failed to apply snapshot to storage: {}", result.error().ToString());
        return std::unexpected(RaftError(RaftErrorCode::ProposalDropped));
    }

    applied_index_ = snapshot.metadata().index();

    return {};
}

void ReadyProcessor::SendMessages(const std::vector<Message>& messages) {
    if (messages.empty()) {
        return;
    }

    transport_.Send(messages);
}

Result<void> ReadyProcessor::ApplyCommittedEntries(const std::vector<Entry>& entries) {
    for (const auto& entry : entries) {
        if (auto result = ApplyEntry(entry); !result) {
            // Log but continue - state machine errors shouldn't stop Raft
            spdlog::warn(
                "Failed to apply entry at index {}: {}", entry.index(), result.error().ToString()
            );
        }
        applied_index_ = entry.index();
    }
    return {};
}

Result<void> ReadyProcessor::ApplyEntry(const Entry& entry) {
    // Handle configuration changes
    if (entry.entry_type() == EntryConfChange || entry.entry_type() == EntryConfChangeV2) {
        ConfChangeV2 cc;
        if (entry.entry_type() == EntryConfChange) {
            // Convert ConfChange to ConfChangeV2
            ConfChange cc_v1;
            if (!cc_v1.ParseFromString(entry.data())) {
                return std::unexpected(RaftError(RaftErrorCode::ProposalDropped));
            }
            auto* single = cc.add_changes();
            single->set_change_type(cc_v1.change_type());
            single->set_node_id(cc_v1.node_id());
            cc.set_context(cc_v1.context());
        } else {
            if (!cc.ParseFromString(entry.data())) {
                return std::unexpected(RaftError(RaftErrorCode::ProposalDropped));
            }
        }

        auto result = raw_node_.ApplyConfChange(cc);
        if (!result) {
            return result.error();
        }

        // Update transport peers based on conf change
        for (const auto& change : cc.changes()) {
            if (change.change_type() == AddNode || change.change_type() == AddLearnerNode) {
                // Note: address needs to be provided via context or external mechanism
                // For now, we skip adding - the user should call AddNode explicitly
                spdlog::info("Node {} added to configuration", change.node_id());
            } else if (change.change_type() == RemoveNode) {
                transport_.RemovePeer(change.node_id());
                spdlog::info("Node {} removed from configuration", change.node_id());
            }
        }

        // Complete the proposal callback if this was a tracked proposal
        if (!cc.context().empty()) {
            proposal_tracker_.Complete(cc.context(), "conf change applied");
        }

        return {};
    }

    // Handle normal entries
    if (entry.data().empty()) {
        // Empty entry after leader election - no callback to complete
        return {};
    }

    auto result = state_machine_.Apply(entry);
    if (!result) {
        // State machine error - still complete the proposal but with error
        if (!entry.context().empty()) {
            proposal_tracker_.Fail(entry.context(), result.error());
        }
        return result.error();
    }

    // Complete the proposal callback
    if (!entry.context().empty()) {
        std::string response = result->response.value_or("");
        proposal_tracker_.Complete(entry.context(), response);
    }

    return {};
}

void ReadyProcessor::ProcessLightReady(const LightReady& light_rd) {
    // Send additional messages
    SendMessages(light_rd.messages);

    // Apply additional committed entries
    for (const auto& entry : light_rd.committed_entries) {
        if (auto result = ApplyEntry(entry); !result) {
            spdlog::warn(
                "Failed to apply entry at index {}: {}", entry.index(), result.error().ToString()
            );
        }
        applied_index_ = entry.index();
    }

    // Update applied index
    if (!light_rd.committed_entries.empty()) {
        raw_node_.AdvanceApply();
    }
}

void ReadyProcessor::CheckLeadershipChange(const Ready& rd) {
    if (!rd.ss) {
        return;
    }

    const auto& ss = *rd.ss;
    bool was_leader = (prev_role_ == StateRole::Leader);
    bool is_leader = (ss.raft_state == StateRole::Leader);

    // Detect leadership change
    if (was_leader != is_leader || prev_leader_ != ss.leader_id ||
        (rd.hs && prev_term_ != rd.hs->term())) {
        uint64_t term = rd.hs ? rd.hs->term() : prev_term_;

        spdlog::info(
            "Leadership change: role={}, leader={}, term={}", static_cast<int>(ss.raft_state),
            ss.leader_id, term
        );

        state_machine_.OnLeadershipChange(is_leader, term, ss.leader_id);

        // If we lost leadership, fail all pending proposals
        if (was_leader && !is_leader) {
            proposal_tracker_.FailAll(RaftError(RaftErrorCode::ProposalDropped));
        }
    }

    prev_role_ = ss.raft_state;
    prev_leader_ = ss.leader_id;
    if (rd.hs) {
        prev_term_ = rd.hs->term();
    }
}

SnapshotData ReadyProcessor::ToSnapshotData(const Snapshot& snapshot) {
    SnapshotData data;
    data.data = std::vector<uint8_t>(snapshot.data().begin(), snapshot.data().end());
    data.metadata = snapshot.metadata();
    return data;
}

}  // namespace raftpp::raftor
