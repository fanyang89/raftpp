#include "raftpp/raft_core.h"

#include "raftpp/util.h"

namespace raftpp {

RaftCore::RaftCore(const Config& config, std::unique_ptr<Storage> store)
    : term_(0),
      vote_(0),
      id_(config.id),
      raft_log_(config, std::move(store)),
      max_inflight_(config.max_inflight_messages),
      max_message_size_(config.max_size_per_message),
      pending_request_snapshot_(INVALID_INDEX),
      state_(StateRole::Follower),
      promotable_(false),
      leader_id_(0),
      pending_conf_index_(0),
      read_only_(config.read_only_option),
      election_elapsed_(0),
      heartbeat_elapsed_(0),
      check_quorum_(config.check_quorum),
      pre_vote_(config.pre_vote),
      skip_broadcast_commit_(config.skip_broadcast_commit),
      batch_append_(config.batch_append),
      disable_proposal_forwarding_(config.disable_proposal_forwarding),
      heartbeat_timeout_(config.heartbeat_tick),
      election_timeout_(config.election_tick),
      randomized_election_timeout_(0),
      min_election_timeout_(0),
      max_election_timeout_(0),
      priority_(0),
      uncommitted_state_(
          UncommittedState{
              .max_uncommitted_size = config.max_uncommitted_size,
              .uncommitted_size = 0,
              .last_log_tail_index = 0
          }
      ),
      max_committed_size_per_ready(config.max_committed_size_per_ready) {}

bool RaftCore::TryBatching(
    const uint64_t to, std::vector<Message>& messages, Progress& pr, const std::vector<Entry>& entries
) const {
    bool is_batched = false;
    for (auto& msg : messages) {
        if (msg.msg_type() == MsgAppend && msg.to() == to) {
            if (!entries.empty()) {
                if (!IsContinuousEntries(msg, entries)) {
                    return is_batched;
                }

                for (const auto& entry : msg.entries()) {
                    msg.mutable_entries()->Add()->CopyFrom(entry);
                }

                for (const auto& entry : entries) {
                    msg.mutable_entries()->Add()->CopyFrom(entry);
                }

                const size_t size = msg.entries().size();
                const uint64_t last_idx = msg.entries().at(size - 1).index();
                pr.UpdateState(last_idx);
            }
            msg.set_commit(raft_log_.committed());
            is_batched = true;
            break;
        }
    }
    return is_batched;
}

void RaftCore::PrepareSendEntries(
    Message& message, Progress& pr, const uint64_t term, const std::vector<Entry>& entries
) const {
    message.set_msg_type(MsgAppend);
    message.set_index(pr.next_idx() - 1);
    message.set_log_term(term);
    for (const auto& ent : entries) {
        message.mutable_entries()->Add()->CopyFrom(ent);
    }
    message.set_commit(raft_log_.committed());
    if (!message.entries().empty()) {
        const uint64_t last_index = message.entries().at(message.entries_size() - 1).index();
        pr.UpdateState(last_index);
    }
}

void RaftCore::SendAppend(const uint64_t to, Progress& pr, std::vector<Message>& messages) {
    MaybeSendAppend(to, pr, true, messages);
}

void RaftCore::SendAppendAggressively(uint64_t to, Progress& pr, std::vector<Message>& messages) {
    while (MaybeSendAppend(to, pr, false, messages)) {}
}

void RaftCore::Send(Message& m, std::vector<Message>& messages) const {
    if (m.from() == INVALID_ID) {
        m.set_from(id_);
    }

    switch (m.msg_type()) {
        case MsgRequestPreVote:
        case MsgRequestPreVoteResponse:
        case MsgRequestVote:
        case MsgRequestVoteResponse:
            if (m.term() == 0) {
                PANIC("term should be set when sending {}", magic_enum::enum_name(m.msg_type()));
            }
            break;
        default:
            break;
    }

    if (m.term() != 0) {
        PANIC("term should not be set when sending {} (was {})", magic_enum::enum_name(m.msg_type()), m.term());
    }

    if (m.msg_type() != MsgPropose || m.msg_type() != MsgReadIndex) {
        m.set_term(term_);
    }

    if (m.msg_type() == MsgRequestVote || m.msg_type() == MsgRequestPreVote) {
        m.set_priority(priority_);
    }

    messages.emplace_back(m);
}

bool RaftCore::PrepareSendSnapshot(Message& m, Progress& pr, uint64_t to) {
    if (!pr.recent_active()) {
        return false;
    }

    m.set_msg_type(MsgSnapshot);

    if (const auto snapshot_r = raft_log_.GetSnapshot(pr.pending_request_snapshot(), to); !snapshot_r) {
        if (snapshot_r.error() == StorageErrorCode::SnapshotTemporarilyUnavailable) {
            return false;
        }
        PANIC("unexpected error: {}", snapshot_r.error());
    } else {
        const auto snapshot = *snapshot_r;
        if (snapshot.metadata().index() == 0) {
            PANIC("need non-empty snapshot");
        }

        const uint64_t s_index = snapshot.metadata().index();
        const uint64_t s_term = snapshot.metadata().term();
        m.mutable_snapshot()->CopyFrom(snapshot);
        pr.BecomeSnapshot(s_index);
        return true;
    }
}

bool RaftCore::MaybeSendAppend(
    const uint64_t to, Progress& pr, const bool allow_empty, std::vector<Message>& messages
) {
    if (pr.IsPaused()) {
        return false;
    }

    Message m;
    m.set_to(to);

    if (pr.pending_request_snapshot() != INVALID_INDEX) {
        if (!PrepareSendSnapshot(m, pr, to)) {
            return false;
        }
    } else {
        GetEntriesContext ctx;
        ctx.what = GetEntriesFor::SendAppend;
        ctx.payload.send_append.to = to;
        ctx.payload.send_append.term = term_;
        ctx.payload.send_append.aggressively = !allow_empty;

        const auto ents = raft_log_.GetEntries(pr.next_idx(), max_message_size_, ctx);

        if (!allow_empty && (!ents || ents->empty())) {
            return false;
        }

        const auto term_r = raft_log_.Term(pr.next_idx() - 1);
        if (term_r && ents) {
            if (batch_append_ && TryBatching(to, messages, pr, *ents)) {
                return true;
            }
            PrepareSendEntries(m, pr, *term_r, *ents);
        } else if (!ents && ents.error().Is(StorageErrorCode::LogTemporarilyUnavailable)) {
            return false;
        } else {
            if (!PrepareSendSnapshot(m, pr, to)) {
                return false;
            }
        }
    }

    Send(m, messages);
    return true;
}

}  // namespace raftpp
