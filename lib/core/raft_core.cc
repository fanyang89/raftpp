#include "raftpp/core/raft_core.h"

#include <spdlog/spdlog.h>

#include "raftpp/core/util.h"

namespace raftpp {

RaftCore::RaftCore(const Config& config, const std::shared_ptr<Storage>& store)
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
      min_election_timeout_(config.MinElectionTick()),
      max_election_timeout_(config.MaxElectionTick()),
      priority_(0),
      uncommitted_state_(
          UncommittedState{
              .max_uncommitted_size = config.max_uncommitted_size,
              .uncommitted_size = 0,
              .last_log_tail_index = 0
          }
      ),
      max_committed_size_per_ready_(config.max_committed_size_per_ready) {}

bool RaftCore::TryBatching(
    const uint64_t to, std::vector<Message>& messages, Progress& pr,
    const std::vector<Entry>& entries
) const {
    bool is_batched = false;
    for (auto& msg : messages) {
        auto msg_reader = msg.reader();
        if (msg_reader.getMsgType() == MessageType::MSG_APPEND && msg_reader.getTo() == to) {
            if (!entries.empty()) {
                if (!IsContinuousEntries(msg, entries)) {
                    return is_batched;
                }

                // Need to rebuild the entries list with Cap'n Proto
                auto existing_entries = msg_reader.getEntries();
                std::vector<Entry> all_entries;
                all_entries.reserve(existing_entries.size() + entries.size());

                // Copy existing entries
                for (const auto& e : existing_entries) {
                    Entry entry;
                    auto entry_builder = entry.builder();
                    entry_builder.setEntryType(e.getEntryType());
                    entry_builder.setTerm(e.getTerm());
                    entry_builder.setIndex(e.getIndex());
                    entry_builder.setData(e.getData());
                    entry_builder.setContext(e.getContext());
                    all_entries.push_back(std::move(entry));
                }

                // Add new entries
                for (const auto& entry : entries) {
                    all_entries.push_back(entry.clone());
                }

                // Rebuild message with new entries
                auto msg_builder = msg.builder();
                auto entries_builder = msg_builder.initEntries(all_entries.size());
                for (size_t i = 0; i < all_entries.size(); ++i) {
                    auto src_reader = all_entries[i].reader();
                    auto dst = entries_builder[i];
                    dst.setEntryType(src_reader.getEntryType());
                    dst.setTerm(src_reader.getTerm());
                    dst.setIndex(src_reader.getIndex());
                    dst.setData(src_reader.getData());
                    dst.setContext(src_reader.getContext());
                }

                const auto size = all_entries.size();
                const uint64_t last_idx = all_entries[size - 1].reader().getIndex();
                pr.UpdateState(last_idx);
            }
            msg.builder().setCommit(raft_log_.committed());
            is_batched = true;
            break;
        }
    }
    return is_batched;
}

void RaftCore::PrepareSendEntries(
    Message& message, Progress& pr, const uint64_t term, const std::vector<Entry>& entries
) const {
    auto msg_builder = message.builder();
    msg_builder.setMsgType(MessageType::MSG_APPEND);
    msg_builder.setIndex(pr.next_idx() - 1);
    msg_builder.setLogTerm(term);

    auto entries_builder = msg_builder.initEntries(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        auto src_reader = entries[i].reader();
        auto dst = entries_builder[i];
        dst.setEntryType(src_reader.getEntryType());
        dst.setTerm(src_reader.getTerm());
        dst.setIndex(src_reader.getIndex());
        dst.setData(src_reader.getData());
        dst.setContext(src_reader.getContext());
    }

    msg_builder.setCommit(raft_log_.committed());
    if (!entries.empty()) {
        const uint64_t last_index = entries[entries.size() - 1].reader().getIndex();
        pr.UpdateState(last_index);
    }
}

void RaftCore::SendAppend(const uint64_t to, Progress& pr, std::vector<Message>& messages) {
    std::ignore = MaybeSendAppend(to, pr, true, messages);
}

void RaftCore::SendAppendAggressively(uint64_t to, Progress& pr, std::vector<Message>& messages) {
    while (MaybeSendAppend(to, pr, false, messages)) {}
}

void RaftCore::Send(Message& m, std::vector<Message>& messages) const {
    auto m_reader = m.reader();
    auto m_builder = m.builder();

    if (m_reader.getFrom() == INVALID_ID) {
        m_builder.setFrom(id_);
    }

    switch (m_reader.getMsgType()) {
        case MessageType::MSG_REQUEST_PRE_VOTE:
        case MessageType::MSG_REQUEST_PRE_VOTE_RESPONSE:
        case MessageType::MSG_REQUEST_VOTE:
        case MessageType::MSG_REQUEST_VOTE_RESPONSE:
            if (m_reader.getTerm() == 0) {
                PANIC(
                    "term should be set when sending {:d}", static_cast<int>(m_reader.getMsgType())
                );
            }
            break;
        default:
            if (m_reader.getTerm() != 0) {
                PANIC(
                    "term should not be set when sending {:d} (was {})",
                    static_cast<int>(m_reader.getMsgType()), m_reader.getTerm()
                );
            }
            // do not attach term to MsgPropose, MsgReadIndex
            // proposals are a way to forward to the leader and
            // should be treated as local message.
            // MsgReadIndex is also forwarded to leader.
            if (m_reader.getMsgType() != MessageType::MSG_PROPOSE &&
                m_reader.getMsgType() != MessageType::MSG_READ_INDEX) {
                m_builder.setTerm(term_);
            }
            break;
    }

    if (m_reader.getMsgType() == MessageType::MSG_REQUEST_VOTE ||
        m_reader.getMsgType() == MessageType::MSG_REQUEST_PRE_VOTE) {
        m_builder.setPriority(priority_);
    }

    messages.emplace_back(std::move(m));
}

bool RaftCore::PrepareSendSnapshot(Message& m, Progress& pr, uint64_t to) {
    if (!pr.recent_active()) {
        return false;
    }

    m.builder().setMsgType(MessageType::MSG_SNAPSHOT);

    auto snapshot_r = raft_log_.GetSnapshot(pr.pending_request_snapshot(), to);
    if (!snapshot_r) {
        if (snapshot_r.error() == StorageErrorCode::SnapshotTemporarilyUnavailable) {
            return false;
        }
        PANIC("unexpected error: {}", snapshot_r.error());
    } else {
        auto snapshot = std::move(snapshot_r).value();
        auto snap_meta = snapshot.reader().getMetadata();
        if (snap_meta.getIndex() == 0) {
            PANIC("need non-empty snapshot");
        }

        const uint64_t s_index = snap_meta.getIndex();
        const uint64_t s_term = snap_meta.getTerm();

        // Set the snapshot in the message
        auto m_builder = m.builder();
        auto snap_builder = m_builder.initSnapshot();
        auto src_reader = snapshot.reader();
        snap_builder.setData(src_reader.getData());

        // Copy metadata
        auto meta_builder = snap_builder.initMetadata();
        meta_builder.setIndex(s_index);
        meta_builder.setTerm(s_term);

        // Copy conf state
        auto src_conf = snap_meta.getConfState();
        auto conf_builder = meta_builder.initConfState();

        auto voters = src_conf.getVoters();
        auto voters_builder = conf_builder.initVoters(voters.size());
        for (size_t i = 0; i < voters.size(); ++i) {
            voters_builder.set(i, voters[i]);
        }

        auto learners = src_conf.getLearners();
        auto learners_builder = conf_builder.initLearners(learners.size());
        for (size_t i = 0; i < learners.size(); ++i) {
            learners_builder.set(i, learners[i]);
        }

        auto voters_out = src_conf.getVotersOutgoing();
        auto voters_out_builder = conf_builder.initVotersOutgoing(voters_out.size());
        for (size_t i = 0; i < voters_out.size(); ++i) {
            voters_out_builder.set(i, voters_out[i]);
        }

        auto learners_next = src_conf.getLearnersNext();
        auto learners_next_builder = conf_builder.initLearnersNext(learners_next.size());
        for (size_t i = 0; i < learners_next.size(); ++i) {
            learners_next_builder.set(i, learners_next[i]);
        }

        conf_builder.setAutoLeave(src_conf.getAutoLeave());

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
    m.builder().setTo(to);

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
