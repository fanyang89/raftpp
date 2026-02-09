#include "ready_processor.h"

#include <algorithm>
#include <cstring>

#include "raftpp/logging.h"
#include "raftpp/raftor/telemetry.h"

namespace raftpp::raftor {
namespace {

class SnapshotDataReader final : public SnapshotReader {
  public:
    explicit SnapshotDataReader(::capnp::Data::Reader data) : data_(data) {}

    Result<size_t> Read(std::span<uint8_t> out) override {
        if (offset_ >= data_.size()) {
            return 0;
        }
        if (out.empty()) {
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

template <typename ListReader, typename ListBuilder>
void CopyUint64List(ListReader src, ListBuilder dst) {
    for (size_t i = 0; i < src.size(); ++i) {
        dst.set(i, src[i]);
    }
}

SnapshotMetadata CloneSnapshotMetadata(msg::SnapshotMetadata::Reader snap_meta) {
    auto metadata = capnp_util::make<msg::SnapshotMetadata>();
    auto meta_builder = capnp_util::builder<msg::SnapshotMetadata>(metadata);
    meta_builder.setIndex(snap_meta.getIndex());
    meta_builder.setTerm(snap_meta.getTerm());

    auto conf_src = snap_meta.getConfState();
    auto conf_dst = meta_builder.initConfState();

    auto voters_src = conf_src.getVoters();
    CopyUint64List(voters_src, conf_dst.initVoters(voters_src.size()));

    auto learners_src = conf_src.getLearners();
    CopyUint64List(learners_src, conf_dst.initLearners(learners_src.size()));

    auto voters_out_src = conf_src.getVotersOutgoing();
    CopyUint64List(voters_out_src, conf_dst.initVotersOutgoing(voters_out_src.size()));

    auto learners_next_src = conf_src.getLearnersNext();
    CopyUint64List(learners_next_src, conf_dst.initLearnersNext(learners_next_src.size()));

    conf_dst.setAutoLeave(conf_src.getAutoLeave());
    return metadata;
}

}  // namespace

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

    telemetry::ScopedSpan span("raftor.ready.process");
    span.span()->SetAttribute("raft.ready.number", static_cast<int64_t>(rd.number));
    span.span()->SetAttribute("raft.ready.entries", static_cast<int64_t>(rd.entries.size()));
    span.span()->SetAttribute(
        "raft.ready.committed_entries", static_cast<int64_t>(rd.light.committed_entries.size())
    );
    span.span()->SetAttribute(
        "raft.ready.light_messages", static_cast<int64_t>(rd.light.messages.size())
    );
    span.span()->SetAttribute(
        "raft.ready.read_states", static_cast<int64_t>(rd.read_states.size())
    );
    span.span()->SetAttribute("raft.ready.must_sync", rd.must_sync);
    span.span()->SetAttribute("raft.ready.has_snapshot", rd.snapshot != nullptr);
    if (rd.ss) {
        span.span()->SetAttribute("raft.role", static_cast<int64_t>(rd.ss->raft_state));
        span.span()->SetAttribute("raft.leader_id", static_cast<int64_t>(rd.ss->leader_id));
    }
    if (rd.hs) {
        auto hs_reader = capnp_util::reader<msg::HardState>(*rd.hs);
        span.span()->SetAttribute("raft.term", static_cast<int64_t>(hs_reader.getTerm()));
        span.span()->SetAttribute("raft.commit", static_cast<int64_t>(hs_reader.getCommit()));
    }

    // Check for leadership changes before processing
    CheckLeadershipChange(rd);

    // 1. Persist entries to WAL
    if (auto result = PersistEntries(rd); !result) {
        telemetry::RecordErrorIf(span.span(), result);
        return result.error();
    }

    // 2. Persist hard state
    if (auto result = PersistHardState(rd); !result) {
        telemetry::RecordErrorIf(span.span(), result);
        return result.error();
    }

    // 3. Apply snapshot if present
    if (auto result = ApplySnapshot(rd); !result) {
        telemetry::RecordErrorIf(span.span(), result);
        return result.error();
    }

    // 4. Send messages
    // For non-leaders (is_persisted_msg=true), messages are in rd.light.messages
    // and should be sent after persisting entries/hard state.
    // rd.Messages() returns empty for non-leaders, so we use rd.light.messages directly.
    SendMessages(rd.light.messages);

    // 5. Apply committed entries to state machine
    if (auto result = ApplyCommittedEntries(rd.light.committed_entries); !result) {
        telemetry::RecordErrorIf(span.span(), result);
        return result.error();
    }

    // 6. Process read states
    EnqueueReadStates(rd.read_states);

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

    telemetry::ScopedSpan span("raftor.ready.persist_entries");
    span.span()->SetAttribute("raft.entry.count", static_cast<int64_t>(rd.entries.size()));

    if (auto result = storage_->Append(rd.entries); !result) {
        RAFTPP_LOG_ERROR("Failed to persist entries: {}", result.error().ToString());
        telemetry::RecordErrorIf(span.span(), result);
        return std::unexpected(RaftError(RaftErrorCode::ProposalDropped));
    }

    return {};
}

Result<void> ReadyProcessor::PersistHardState(const Ready& rd) {
    if (!rd.hs) {
        return {};
    }

    telemetry::ScopedSpan span("raftor.ready.persist_hard_state");
    auto hs_reader = capnp_util::reader<msg::HardState>(*rd.hs);
    span.span()->SetAttribute("raft.term", static_cast<int64_t>(hs_reader.getTerm()));
    span.span()->SetAttribute("raft.commit", static_cast<int64_t>(hs_reader.getCommit()));

    // SetHardState doesn't return error in current implementation
    storage_->SetHardState(CloneHardState(*rd.hs));

    // Sync if required
    if (rd.must_sync) {
        if (auto result = storage_->Sync(); !result) {
            RAFTPP_LOG_ERROR("Failed to sync storage: {}", result.error().ToString());
            telemetry::RecordErrorIf(span.span(), result);
            return std::unexpected(RaftError(RaftErrorCode::ProposalDropped));
        }
    }

    return {};
}

Result<void> ReadyProcessor::ApplySnapshot(const Ready& rd) {
    const auto& snapshot = rd.snapshot;
    if (!snapshot) {
        return {};  // No snapshot
    }
    auto snap_reader = capnp_util::reader<msg::Snapshot>(snapshot);
    auto snap_meta = snap_reader.getMetadata();

    if (snap_meta.getIndex() == 0) {
        return {};  // No snapshot
    }

    telemetry::ScopedSpan span("raftor.ready.apply_snapshot");
    span.span()->SetAttribute("raft.snapshot.index", static_cast<int64_t>(snap_meta.getIndex()));
    span.span()->SetAttribute("raft.snapshot.term", static_cast<int64_t>(snap_meta.getTerm()));

    RAFTPP_LOG_INFO(
        "Applying snapshot at index {} term {}", snap_meta.getIndex(), snap_meta.getTerm()
    );

    // First restore to state machine.
    auto metadata = CloneSnapshotMetadata(snap_meta);
    SnapshotDataReader reader(snap_reader.getData());
    if (auto result = state_machine_.RestoreSnapshot(metadata, reader); !result) {
        RAFTPP_LOG_ERROR("Failed to restore snapshot to state machine");
        telemetry::RecordErrorIf(span.span(), result);
        return result.error();
    }

    // Then apply to storage
    if (auto result = storage_->ApplySnapshot(snapshot); !result) {
        RAFTPP_LOG_ERROR("Failed to apply snapshot to storage: {}", result.error().ToString());
        telemetry::RecordErrorIf(span.span(), result);
        return std::unexpected(RaftError(RaftErrorCode::ProposalDropped));
    }

    applied_index_ = snap_meta.getIndex();
    MaybeCompletePendingReads();

    return {};
}

void ReadyProcessor::SendMessages(const std::vector<Message>& messages) {
    if (messages.empty()) {
        return;
    }

    telemetry::ScopedSpan span("raftor.ready.send_messages");
    span.span()->SetAttribute("raft.message.count", static_cast<int64_t>(messages.size()));

    transport_.Send(messages);
}

Result<void> ReadyProcessor::ApplyCommittedEntries(const std::vector<Entry>& entries) {
    telemetry::ScopedSpan span("raftor.ready.apply_entries");
    span.span()->SetAttribute("raft.entry.count", static_cast<int64_t>(entries.size()));

    bool has_error = false;
    for (const auto& entry : entries) {
        if (auto result = ApplyEntry(entry); !result) {
            // Log but continue - state machine errors shouldn't stop Raft
            auto entry_reader = capnp_util::reader<msg::Entry>(entry);
            RAFTPP_LOG_WARN(
                "Failed to apply entry at index {}: {}", entry_reader.getIndex(),
                result.error().ToString()
            );
            has_error = true;
        }
        applied_index_ = capnp_util::reader<msg::Entry>(entry).getIndex();
    }
    if (has_error) {
        telemetry::RecordError(span.span(), "committed entry apply failed");
    }
    MaybeCompletePendingReads();
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
                auto ctx = cc_reader.getContext();
                std::string addr;
                if (ctx.size() > 0) {
                    addr.assign(reinterpret_cast<const char*>(ctx.begin()), ctx.size());
                }
                if (addr.empty()) {
                    RAFTPP_LOG_INFO("Node {} added to configuration", change.getNodeId());
                } else {
                    RAFTPP_LOG_INFO(
                        "Node {} added to configuration (address: {})", change.getNodeId(), addr
                    );
                }
            } else if (change.getChangeType() == ConfChangeType::REMOVE_NODE) {
                transport_.RemovePeer(change.getNodeId());
                RAFTPP_LOG_INFO("Node {} removed from configuration", change.getNodeId());
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
    telemetry::ScopedSpan span("raftor.ready.process_light");
    span.span()->SetAttribute("raft.message.count", static_cast<int64_t>(light_rd.messages.size()));
    span.span()->SetAttribute(
        "raft.entry.count", static_cast<int64_t>(light_rd.committed_entries.size())
    );

    // Send additional messages
    SendMessages(light_rd.messages);

    // Apply additional committed entries
    for (const auto& entry : light_rd.committed_entries) {
        if (auto result = ApplyEntry(entry); !result) {
            auto entry_reader = capnp_util::reader<msg::Entry>(entry);
            RAFTPP_LOG_WARN(
                "Failed to apply entry at index {}: {}", entry_reader.getIndex(),
                result.error().ToString()
            );
            telemetry::RecordErrorIf(span.span(), result);
        }
        applied_index_ = capnp_util::reader<msg::Entry>(entry).getIndex();
    }
    MaybeCompletePendingReads();

    // Update applied index
    if (!light_rd.committed_entries.empty()) {
        raw_node_.AdvanceApply();
    }
}

void ReadyProcessor::EnqueueReadStates(const std::vector<ReadState>& read_states) {
    if (read_states.empty()) {
        return;
    }

    telemetry::ScopedSpan span("raftor.ready.enqueue_read_states");
    span.span()->SetAttribute("raft.read_states.count", static_cast<int64_t>(read_states.size()));

    pending_reads_.reserve(pending_reads_.size() + read_states.size());
    for (const auto& rs : read_states) {
        pending_reads_.push_back(PendingRead{rs.index, rs.request_ctx});
    }
    MaybeCompletePendingReads();
}

void ReadyProcessor::MaybeCompletePendingReads() {
    if (pending_reads_.empty()) {
        return;
    }

    // This is on a hot path: avoid allocating a new vector on every invocation.
    const auto applied_index = applied_index_;
    auto new_end = std::remove_if(
        pending_reads_.begin(), pending_reads_.end(), [&](const PendingRead& pending) {
            if (!proposal_tracker_.IsReadPending(pending.ctx)) {
                return true;
            }
            if (applied_index >= pending.index) {
                proposal_tracker_.CompleteRead(pending.ctx);
                return true;
            }
            return false;
        }
    );
    pending_reads_.erase(new_end, pending_reads_.end());
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

        telemetry::ScopedSpan span("raftor.leadership_change");
        span.span()->SetAttribute("raft.role", static_cast<int64_t>(ss.raft_state));
        span.span()->SetAttribute("raft.leader_id", static_cast<int64_t>(ss.leader_id));
        span.span()->SetAttribute("raft.term", static_cast<int64_t>(term));
        span.span()->SetAttribute("raft.was_leader", was_leader);
        span.span()->SetAttribute("raft.is_leader", is_leader);

        RAFTPP_LOG_INFO(
            "Leadership change: role={}, leader={}, term={}", static_cast<int>(ss.raft_state),
            ss.leader_id, term
        );

        state_machine_.OnLeadershipChange(is_leader, term, ss.leader_id);

        // If we lost leadership, fail all pending requests.
        //
        // Pending proposals should be dropped because they can no longer be committed by us.
        // Pending reads should fail because read index requests issued under our leadership may
        // never complete once we step down.
        if (was_leader && !is_leader) {
            proposal_tracker_.FailAll(RaftError(RaftErrorCode::ProposalDropped));
            proposal_tracker_.FailAllReads(RaftError(RaftErrorCode::LostLeadership));
        }
    }

    prev_role_ = ss.raft_state;
    prev_leader_ = ss.leader_id;
    if (rd.hs) {
        prev_term_ = capnp_util::reader<msg::HardState>(*rd.hs).getTerm();
    }
}

}  // namespace raftpp::raftor
