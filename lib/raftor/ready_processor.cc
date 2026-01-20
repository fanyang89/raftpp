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
    storage_->SetHardState(CloneHardState(*rd.hs));

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
    auto snap_reader = capnp_util::reader<msg::Snapshot>(snapshot);
    auto snap_meta = snap_reader.getMetadata();

    if (snap_meta.getIndex() == 0) {
        return {};  // No snapshot
    }

    spdlog::info(
        "Applying snapshot at index {} term {}", snap_meta.getIndex(), snap_meta.getTerm()
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

    applied_index_ = snap_meta.getIndex();

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
            auto entry_reader = capnp_util::reader<msg::Entry>(entry);
            spdlog::warn(
                "Failed to apply entry at index {}: {}", entry_reader.getIndex(),
                result.error().ToString()
            );
        }
        applied_index_ = capnp_util::reader<msg::Entry>(entry).getIndex();
    }
    return {};
}

Result<void> ReadyProcessor::ApplyEntry(const Entry& entry) {
    auto entry_reader = capnp_util::reader<msg::Entry>(entry);

    // Handle configuration changes
    if (entry_reader.getEntryType() == EntryType::ENTRY_CONF_CHANGE ||
        entry_reader.getEntryType() == EntryType::ENTRY_CONF_CHANGE_V2) {
        ConfChangeV2 cc = capnp_util::make<msg::ConfChangeV2>();

        if (entry_reader.getEntryType() == EntryType::ENTRY_CONF_CHANGE) {
            // Convert ConfChange to ConfChangeV2
            ConfChange cc_v1;
            auto data = entry_reader.getData();

            try {
                const ::capnp::word* words = reinterpret_cast<const ::capnp::word*>(data.begin());
                size_t word_count = data.size() / sizeof(::capnp::word);
                cc_v1 = capnp_util::fromBytes<msg::ConfChange>(
                    std::span<const uint8_t>(data.begin(), data.size())
                );
            } catch (...) {
                return std::unexpected(RaftError(RaftErrorCode::ProposalDropped));
            }

            auto cc_v1_reader = capnp_util::reader<msg::ConfChange>(cc_v1);
            auto cc_builder = capnp_util::builder<msg::ConfChangeV2>(cc);
            auto single = cc_builder.initChanges(1)[0];
            single.setChangeType(cc_v1_reader.getChangeType());
            single.setNodeId(cc_v1_reader.getNodeId());
            cc_builder.setContext(cc_v1_reader.getContext());
        } else {
            auto data = entry_reader.getData();

            try {
                cc = capnp_util::fromBytes<msg::ConfChangeV2>(
                    std::span<const uint8_t>(data.begin(), data.size())
                );
            } catch (...) {
                return std::unexpected(RaftError(RaftErrorCode::ProposalDropped));
            }
        }

        auto result = raw_node_.ApplyConfChange(cc);
        if (!result) {
            return result.error();
        }

        // Update transport peers based on conf change
        auto cc_reader = capnp_util::reader<msg::ConfChangeV2>(cc);
        auto changes = cc_reader.getChanges();
        for (const auto& change : changes) {
            if (change.getChangeType() == ConfChangeType::ADD_NODE ||
                change.getChangeType() == ConfChangeType::ADD_LEARNER_NODE) {
                // Note: address needs to be provided via context or external mechanism
                // For now, we skip adding - the user should call AddNode explicitly
                spdlog::info("Node {} added to configuration", change.getNodeId());
            } else if (change.getChangeType() == ConfChangeType::REMOVE_NODE) {
                transport_.RemovePeer(change.getNodeId());
                spdlog::info("Node {} removed from configuration", change.getNodeId());
            }
        }

        // Complete the proposal callback if this was a tracked proposal
        auto context = cc_reader.getContext();
        if (context.size() > 0) {
            std::string ctx_str(reinterpret_cast<const char*>(context.begin()), context.size());
            proposal_tracker_.Complete(ctx_str, "conf change applied");
        }

        return {};
    }

    // Handle normal entries
    auto data = entry_reader.getData();
    if (data.size() == 0) {
        // Empty entry after leader election - no callback to complete
        return {};
    }

    auto result = state_machine_.Apply(entry);
    if (!result) {
        // State machine error - still complete the proposal but with error
        auto context = entry_reader.getContext();
        if (context.size() > 0) {
            std::string ctx_str(reinterpret_cast<const char*>(context.begin()), context.size());
            proposal_tracker_.Fail(ctx_str, result.error());
        }
        return result.error();
    }

    // Complete the proposal callback
    auto context = entry_reader.getContext();
    if (context.size() > 0) {
        std::string ctx_str(reinterpret_cast<const char*>(context.begin()), context.size());
        std::string response = result->response.value_or("");
        proposal_tracker_.Complete(ctx_str, response);
    }

    return {};
}

void ReadyProcessor::ProcessLightReady(const LightReady& light_rd) {
    // Send additional messages
    SendMessages(light_rd.messages);

    // Apply additional committed entries
    for (const auto& entry : light_rd.committed_entries) {
        if (auto result = ApplyEntry(entry); !result) {
            auto entry_reader = capnp_util::reader<msg::Entry>(entry);
            spdlog::warn(
                "Failed to apply entry at index {}: {}", entry_reader.getIndex(),
                result.error().ToString()
            );
        }
        applied_index_ = capnp_util::reader<msg::Entry>(entry).getIndex();
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
    uint64_t hs_term = prev_term_;
    if (rd.hs) {
        hs_term = capnp_util::reader<msg::HardState>(*rd.hs).getTerm();
    }

    if (was_leader != is_leader || prev_leader_ != ss.leader_id ||
        (rd.hs && prev_term_ != hs_term)) {
        uint64_t term = rd.hs ? capnp_util::reader<msg::HardState>(*rd.hs).getTerm() : prev_term_;

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
        prev_term_ = capnp_util::reader<msg::HardState>(*rd.hs).getTerm();
    }
}

SnapshotData ReadyProcessor::ToSnapshotData(const Snapshot& snapshot) {
    SnapshotData data;
    auto snap_reader = capnp_util::reader<msg::Snapshot>(snapshot);
    auto snap_data = snap_reader.getData();
    data.data = std::vector<uint8_t>(snap_data.begin(), snap_data.end());

    // Clone the metadata
    auto snap_meta = snap_reader.getMetadata();
    auto meta_builder = capnp_util::builder<msg::SnapshotMetadata>(data.metadata);
    meta_builder.setIndex(snap_meta.getIndex());
    meta_builder.setTerm(snap_meta.getTerm());

    // Copy ConfState
    auto conf_src = snap_meta.getConfState();
    auto conf_dst = meta_builder.initConfState();

    auto voters_src = conf_src.getVoters();
    auto voters_dst = conf_dst.initVoters(voters_src.size());
    for (size_t i = 0; i < voters_src.size(); ++i) {
        voters_dst.set(i, voters_src[i]);
    }

    auto learners_src = conf_src.getLearners();
    auto learners_dst = conf_dst.initLearners(learners_src.size());
    for (size_t i = 0; i < learners_src.size(); ++i) {
        learners_dst.set(i, learners_src[i]);
    }

    auto voters_out_src = conf_src.getVotersOutgoing();
    auto voters_out_dst = conf_dst.initVotersOutgoing(voters_out_src.size());
    for (size_t i = 0; i < voters_out_src.size(); ++i) {
        voters_out_dst.set(i, voters_out_src[i]);
    }

    auto learners_next_src = conf_src.getLearnersNext();
    auto learners_next_dst = conf_dst.initLearnersNext(learners_next_src.size());
    for (size_t i = 0; i < learners_next_src.size(); ++i) {
        learners_next_dst.set(i, learners_next_src[i]);
    }

    conf_dst.setAutoLeave(conf_src.getAutoLeave());

    return data;
}

}  // namespace raftpp::raftor
