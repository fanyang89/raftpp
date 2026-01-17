#include "raftpp/core/raft.h"

#include <random>
#include <ranges>

#include <google/protobuf/util/message_differencer.h>

#include "raftpp/core/conf_changer.h"
#include "raftpp/core/conf_restore.h"
#include "spdlog/spdlog.h"

namespace raftpp {

constexpr std::string_view CAMPAIGN_PRE_ELECTION = "CampaignPreElection";
constexpr std::string_view CAMPAIGN_ELECTION = "CampaignElection";
constexpr std::string_view CAMPAIGN_TRANSFER = "CampaignTransfer";

bool UncommittedState::IsNoLimit() const {
    return max_uncommitted_size == std::numeric_limits<size_t>::max();
}

bool UncommittedState::MaybeIncreaseUncommittedSize(std::span<const Entry> entries) {
    if (IsNoLimit()) {
        return true;
    }

    const std::size_t size = std::transform_reduce(
        entries.begin(), entries.end(), std::size_t{0}, std::plus{},
        [](const Entry& e) { return e.data().size(); }
    );

    if (size == 0 || uncommitted_size == 0 || size + uncommitted_size <= max_uncommitted_size) {
        uncommitted_size += size;
        return true;
    }

    return false;
}

bool UncommittedState::MaybeReduceUncommittedSize(std::span<const Entry> entries) {
    if (IsNoLimit() || entries.empty()) {
        return true;
    }

    const std::size_t size = std::ranges::fold_left(
        entries | std::views::drop_while([this](const Entry& e) {
            return e.index() <= last_log_tail_index;
        }) | std::views::transform([](const Entry& e) { return e.data().size(); }),
        std::size_t{0}, std::plus{}
    );

    if (size > uncommitted_size) {
        uncommitted_size = 0;
        return false;
    }

    uncommitted_size -= size;
    return true;
}

Raft::Raft(const Config& config, const std::shared_ptr<Storage>& store)
    : RaftCore(config, std::move(store)),
      progress_tracker_(config.max_inflight_messages),
      config_(config) {
    if (const auto r = config.Validate(); !r) {
        PANIC(r.error());
    }
    const auto raft_state_result = raft_log_.GetInitialState();
    if (!raft_state_result) {
        PANIC(raft_state_result.error());
    }
    const auto& raft_state = *raft_state_result;

    auto& conf_state = raft_state.conf_state;

    if (const auto r = raftpp::Restore(progress_tracker_, raft_log_.LastIndex(), conf_state); !r) {
        PANIC("Configuration restore failed, err: {}", r.error());
    }

    if (const ConfState new_cs = PostConfChange();
        !ConfStatesEqualIgnoringOrder(conf_state, new_cs)) {
        PANIC("invalid restore: {} != {}", conf_state.DebugString(), new_cs.DebugString());
    }

    // Only load hard state if it's explicitly configured (for production use)
    // In tests, we start with default state to match raft-rs behavior
    if (config.load_state_on_startup &&
        !google::protobuf::util::MessageDifferencer::Equals(
            raft_state.hard_state, HardState::default_instance()
        )) {
        LoadState(raft_state.hard_state);
    }

    if (config.applied > 0) {
        CommitApplyInternal(config.applied, true);
    }

    BecomeFollower(term_, INVALID_ID);

    RaftLog& log = raft_log_;
    SPDLOG_INFO(
        "new raft instance, term={}, commit={}, applied={}, last_index={}, last_term={}, peers={}",
        term_, log.committed(), log.applied(), log.LastIndex(), log.LastTerm(),
        fmt::format("{}", progress_tracker_.conf().voters)
    );
}

ConfState Raft::PostConfChange() {
    // TODO(fanyang) formatter for tracker conf
    // SPDLOG_INFO("switched to configuration, config={}", progress_tracker_.conf());
    const auto cs = progress_tracker_.conf().ToConfState();
    const bool is_voter = progress_tracker_.conf().voters.Contains(id_);
    promotable_ = is_voter;

    if (!is_voter && state_ == StateRole::Leader) {
        return cs;
    }

    if (state_ != StateRole::Leader || cs.voters().empty()) {
        return cs;
    }

    if (MaybeCommit()) {
        BroadcastAppend();
    } else {
        for (auto& [id, p] : progress_tracker_.progress_map()) {
            if (id == id_) {
                continue;
            }
            std::ignore = MaybeSendAppend(id, p, false, messages_);
        }
    }

    if (const auto ctx = read_only_.LastPendingRequestCtx()) {
        if (const auto acks = read_only_.RecvACK(id_, *ctx);
            acks && progress_tracker_.HasQuorum(*acks)) {
            for (const auto& rs : read_only_.Advance(*ctx)) {
                if (auto m = HandleReadyReadIndex(rs.req, rs.index)) {
                    Send(*m, messages_);
                }
            }
        }
    }

    if (lead_transferee_ && !progress_tracker_.conf().voters.Contains(*lead_transferee_)) {
        AbortLeaderTransfer();
    }

    return cs;
}

void Raft::LoadState(const HardState& hs) {
    if (hs.commit() < raft_log_.committed() || hs.commit() > raft_log_.LastIndex()) {
        PANIC(
            "hs.commit {} is out of range [{}, {}]", hs.commit(), raft_log_.committed(),
            raft_log_.LastIndex()
        );
    }
    raft_log_.committed() = hs.commit();
    term_ = hs.term();
    vote_ = hs.vote();
}

bool Raft::MaybeIncreaseUncommittedSize(const std::span<const Entry> entries) {
    return uncommitted_state_.MaybeIncreaseUncommittedSize(entries);
}

bool Raft::AppendEntry(const Entry& entry) {
    return AppendEntry(std::vector<Entry>{entry});
}

bool Raft::AppendEntry(std::vector<Entry> entries) {
    if (!MaybeIncreaseUncommittedSize(entries)) {
        return false;
    }

    const uint64_t last_index = raft_log_.LastIndex();
    for (size_t i = 0; i < entries.size(); ++i) {
        auto& entry = entries[i];
        entry.set_term(term_);
        entry.set_index(last_index + i + 1);
    }

    std::ignore = raft_log_.Append(entries);
    return true;
}

bool Raft::MaybeCommit() {
    const auto max_commit_index = progress_tracker_.MaxCommittedIndex().first;
    if (raft_log_.MaybeCommit(max_commit_index, term_)) {
        const uint64_t self_id = id_;
        const uint64_t committed = raft_log_.committed();
        progress_tracker_.at(self_id).UpdateCommitted(committed);
        return true;
    }
    return false;
}

bool Raft::ShouldBroadcastCommit() const {
    return !skip_broadcast_commit_ || HasPendingConf();
}

bool Raft::HasPendingConf() const {
    return pending_conf_index_ > raft_log_.applied();
}

void Raft::BroadcastAppend() {
    const auto self_id = id_;
    auto& messages = messages_;
    for (auto& [id, pr] : progress_tracker_.progress_map()) {
        if (id == self_id) {
            continue;
        }
        RaftCore::SendAppend(id, pr, messages);
    }
}

void Raft::OnPersistEntries(const uint64_t index, const uint64_t term) {
    const bool update = raft_log_.MaybePersist(index, term);
    if (update && state_ == StateRole::Leader) {
        if (term_ != term) {
            SPDLOG_ERROR(
                "leader's persisted index changed but the term {} is not the same as {}", term,
                term_
            );
        }

        const uint64_t self_id = id_;
        Progress& pr = progress_tracker_.at(self_id);
        if (pr.MaybeUpdate(index) && MaybeCommit() && ShouldBroadcastCommit()) {
            BroadcastAppend();
        }
    }
}

void Raft::OnPersistSnapshot(const uint64_t index) {
    std::ignore = raft_log_.MaybePersistSnapshot(index);
}

void Raft::BecomePreCandidate() {
    ASSERT(state_ != StateRole::Leader, "invalid transition [leader -> pre-candidate]");
    state_ = StateRole::PreCandidate;
    progress_tracker_.ResetVotes();
    leader_id_ = INVALID_ID;
    SPDLOG_INFO("became pre-candidate, term={}", term_);
}

void Raft::BecomeCandidate() {
    ASSERT(state_ != StateRole::Leader, "invalid transition [leader -> candidate]");
    const auto term = term_ + 1;
    Reset(term);
    const auto id = id_;
    vote_ = id;
    state_ = StateRole::Candidate;
    promotable_ = progress_tracker_.conf().voters.Contains(id);
    SPDLOG_INFO("became candidate, term={}", term_);
}

void Raft::BecomeLeader() {
    ASSERT(state_ != StateRole::Follower, "invalid transition [follower -> leader]");

    Reset(term_);
    leader_id_ = id_;
    state_ = StateRole::Leader;

    const uint64_t last_index = raft_log_.LastIndex();

    // Update uncommitted state
    uncommitted_state_.uncommitted_size = 0;
    uncommitted_state_.last_log_tail_index = last_index;

    progress_tracker_.at(id_).BecomeReplicate();
    pending_conf_index_ = last_index;

    if (!AppendEntry(Entry())) {
        PANIC("appending an empty entry should never be dropped");
    }

    SPDLOG_INFO("became leader at term {}", term_);
}

VoteResult Raft::Poll(const uint64_t from, MessageType mt, const bool vote) {
    progress_tracker_.RecordVote(from, vote);
    const auto& r = progress_tracker_.CountVotes();
    if (from != id_) {
        SPDLOG_DEBUG("received votes response from {}, vote={}", from, vote);
    }

    switch (r.result) {
        case VoteResult::Pending:
            break;
        case VoteResult::Lost:
            BecomeFollower(term_, INVALID_ID);
            break;
        case VoteResult::Won:
            if (state_ == StateRole::PreCandidate) {
                Campaign(CAMPAIGN_ELECTION);
            } else {
                BecomeLeader();
                BroadcastAppend();
            }
            break;
    }

    return r.result;
}

void Raft::Campaign(std::string_view campaign_type) {
    MessageType vote_msg;
    uint64_t term;

    if (campaign_type == CAMPAIGN_PRE_ELECTION) {
        BecomePreCandidate();
        vote_msg = MsgRequestPreVote;
        term = term_ + 1;
    } else {
        BecomeCandidate();
        vote_msg = MsgRequestVote;
        term = term_;
    }

    const auto self_id = id_;
    if (Poll(self_id, vote_msg, true) == VoteResult::Won) {
        return;
    }

    const auto [commit, commit_term] = raft_log_.CommitInfo();
    std::vector<uint64_t> voters;

    // Only send vote request to voters.
    for (const uint64_t id : progress_tracker_.conf().voters.IDs()) {
        if (id == self_id) {
            continue;
        }

        Message m;
        m.set_to(id);
        m.set_msg_type(vote_msg);
        m.set_term(term);
        m.set_index(raft_log_.LastIndex());
        m.set_log_term(raft_log_.LastTerm());
        m.set_commit(commit);
        m.set_commit_term(commit_term);
        if (campaign_type == CAMPAIGN_TRANSFER) {
            m.set_context(campaign_type);
        }

        Send(m, messages_);
    }
}

void Raft::Hup(const bool transfer_leader) {
    if (state_ == StateRole::Leader) {
        SPDLOG_DEBUG("ignoring MsgHup because already leader");
        return;
    }

    uint64_t low;
    if (const auto idx = raft_log_.unstable().MaybeFirstIndex()) {
        low = *idx;
    } else {
        low = raft_log_.applied() + 1;
    }

    const auto high = raft_log_.committed() + 1;
    GetEntriesContext ctx;
    ctx.what = GetEntriesFor::TransferLeader;
    if (HasUnappliedConfChanges(low, high, ctx)) {
        SPDLOG_WARN(
            "cannot campaign at term {} since there are still pending configuration changes to "
            "apply",
            term_
        );
        return;
    }

    SPDLOG_INFO("starting a new election, term={}", term_);
    if (transfer_leader) {
        Campaign(CAMPAIGN_TRANSFER);
    } else if (pre_vote_) {
        Campaign(CAMPAIGN_PRE_ELECTION);
    } else {
        Campaign(CAMPAIGN_ELECTION);
    }
}

bool Raft::HasUnappliedConfChanges(uint64_t low, uint64_t high, const GetEntriesContext& ctx) {
    if (raft_log_.applied() >= raft_log_.committed()) {
        // in fact applied == committed
        return false;
    }

    bool found = false;
    const auto page_size = max_committed_size_per_ready_;

    const auto scanFn = [&found](const std::vector<Entry>& ents) -> bool {
        for (const auto& e : ents) {
            if (e.entry_type() == EntryConfChangeV2) {
                found = true;
                return false;
            }
        }
        return true;
    };

    if (const auto r = raft_log_.Scan(low, high, page_size, ctx, scanFn); !r) {
        PANIC("error scanning unapplied entries [{}, {}): {:?}", low, high, r.error());
    }

    return found;
}

void Raft::CommitApplyInternal(uint64_t applied, bool skip_check) {
    const uint64_t old_applied = raft_log_.applied();
    if (!skip_check) {
        raft_log_.AppliedTo(applied);
    } else {
        ASSERT(applied > 0);
        raft_log_.AppliedToUnchecked(applied);
    }

    if (progress_tracker_.conf().auto_leave && old_applied <= pending_conf_index_ &&
        applied >= pending_conf_index_ && state_ == StateRole::Leader) {
        Entry ent;
        ent.set_entry_type(EntryConfChangeV2);
        if (!AppendEntry(ent)) {
            PANIC("appending an empty EntryConfChangeV2 should never be dropped");
        }

        pending_conf_index_ = raft_log_.LastIndex();
    }
}

void Raft::MaybeCommitByVote(const Message& m) {
    if (m.commit() == 0 || m.commit_term() == 0) {
        return;
    }

    const uint64_t last_commit = raft_log_.committed();
    if (m.commit() <= last_commit || state_ == StateRole::Leader) {
        return;
    }
    if (!raft_log_.MaybeCommit(m.commit(), m.commit_term())) {
        return;
    }

    const auto& log = raft_log_;
    SPDLOG_INFO(
        "[commit: {}, last_index: {}, last_term: {}] fast-forwarded commit to vote request [index: "
        "{}, term: {}]",
        log.committed(), log.LastIndex(), log.LastTerm(), m.commit(), m.commit_term()
    );

    if (state_ != StateRole::Candidate && state_ != StateRole::PreCandidate) {
        return;
    }

    // Scan all unapplied committed entries to find a config change.
    // Paginate the scan, to avoid a potentially unlimited memory spike.
    const uint64_t low = last_commit + 1;
    const uint64_t high = raft_log_.committed() + 1;
    if (constexpr auto ctx = GetEntriesContext(GetEntriesFor::CommitByVote);
        HasUnappliedConfChanges(low, high, ctx)) {
        // The candidate doesn't have to step down in theory, here just for best
        // safety as we assume quorum won't change during election.
        const auto term = term_;
        BecomeFollower(term, INVALID_ID);
    }
}

void Raft::SendTimeoutNow(const uint64_t to) {
    Message m;
    m.set_to(to);
    m.set_msg_type(MsgTimeoutNow);
    Send(m, messages_);
}

void Raft::HandleAppendResponse(const Message& m) {
    auto next_probe_index = m.reject_hint();
    // pull out find_conflict_by_term for immutable borrow
    if (m.reject() && m.log_term() > 0) {
        next_probe_index = raft_log_.FindConflictByTerm(m.reject_hint(), m.log_term()).first;
    }

    auto* p = progress_tracker_.get(m.from());
    if (p == nullptr) {
        SPDLOG_WARN("no progress available for {}", m.from());
        return;
    }

    Progress& pr = *p;
    pr.recent_active() = true;
    SPDLOG_DEBUG(
        "HandleAppendResponse: from={}, index={}, reject={}", m.from(), m.index(), m.reject()
    );
    pr.UpdateCommitted(m.commit());

    if (m.reject()) {
        SPDLOG_DEBUG(
            "HandleAppendResponse: reject from {}, index={}, reject_hint={}, log_term={}", m.from(),
            m.index(), m.reject_hint(), m.log_term()
        );
        if (pr.MaybeDecTo(m.index(), next_probe_index, m.request_snapshot())) {
            if (pr.state() == ProgressState::Replicate) {
                pr.BecomeProbe();
            }
            SendAppend(m.from());
        }
        return;
    }

    auto old_paused = pr.IsPaused();
    if (!pr.MaybeUpdate(m.index())) {
        return;
    }

    switch (pr.state()) {
        case ProgressState::Probe:
            pr.BecomeReplicate();
            break;
        case ProgressState::Replicate:
            pr.inflights().FreeTo(m.index());
            if (pr.IsSnapshotCaughtUp()) {
                pr.BecomeProbe();
            }
            break;
        case ProgressState::Snapshot:
            if (pr.IsSnapshotCaughtUp()) {
                pr.BecomeProbe();
            }
            break;
    }

    if (MaybeCommit()) {
        if (ShouldBroadcastCommit()) {
            BroadcastAppend();
        }
    } else if (old_paused) {
        SendAppend(m.from());
    }

    SendAppendAggressively(m.from());

    if (m.from() == lead_transferee_) {
        if (progress_tracker_.at(m.from()).matched() == raft_log_.LastIndex()) {
            SPDLOG_INFO("sent MsgTimeoutNow to {} after received MsgAppResp", m.from());
            SendTimeoutNow(m.from());
        }
    }
}

void Raft::SendRequestSnapshot() {
    Message m;
    m.set_msg_type(MsgAppendResponse);
    m.set_index(raft_log_.committed());
    m.set_reject(true);
    m.set_reject_hint(raft_log_.LastIndex());
    m.set_to(leader_id_);
    m.set_request_snapshot(pending_request_snapshot_);
    m.set_log_term(*raft_log_.Term(m.reject_hint()));

    Send(m, messages_);
}

void Raft::HandleHeartbeat(const Message& m) {
    std::ignore = raft_log_.CommitTo(m.commit());
    if (pending_request_snapshot_ != INVALID_INDEX) {
        SendRequestSnapshot();
        return;
    }

    Message to_send;
    to_send.set_to(m.from());
    to_send.set_msg_type(MsgHeartbeatResponse);
    to_send.set_context(m.context());
    to_send.set_commit(raft_log_.committed());
    Send(to_send, messages_);
}

bool Raft::Restore(const Snapshot& snapshot) {
    if (snapshot.metadata().index() < raft_log_.committed()) {
        return false;
    }

    if (state_ != StateRole::Follower) {
        SPDLOG_WARN("non-follower attempted to restore snapshot, state={}", format_as(state_));
        BecomeFollower(term_ + 1, INVALID_ID);
        return false;
    }

    const auto meta = snapshot.metadata();
    const auto cs = meta.conf_state();

    Set<uint64_t> cs_ids;
    for (const auto voter : cs.voters()) {
        cs_ids.insert(voter);
    }
    for (const auto voter : cs.learners()) {
        cs_ids.insert(voter);
    }
    for (const auto voter : cs.voters_outgoing()) {
        cs_ids.insert(voter);
    }
    if (!cs_ids.contains(id_)) {
        SPDLOG_WARN(
            "attempted to restore snapshot but it is not in the ConfState, cs={}",
            cs.ShortDebugString()
        );
        return false;
    }

    if (pending_request_snapshot_ == INVALID_INDEX &&
        raft_log_.MatchTerm(meta.index(), meta.term())) {
        SPDLOG_INFO("fast-forwarded commit to snapshot");
        std::ignore = raft_log_.CommitTo(meta.index());
        return false;
    }

    std::ignore = raft_log_.Restore(snapshot);

    pending_request_snapshot_ = INVALID_INDEX;

    SPDLOG_INFO("restored snapshot");
    return true;
}

void Raft::HandleSnapshot(const Message& m) {
    Message to_send;
    to_send.set_msg_type(MsgAppendResponse);
    to_send.set_to(m.from());

    if (Restore(m.snapshot())) {
        to_send.set_index(raft_log_.LastIndex());
    } else {
        to_send.set_index(raft_log_.committed());
    }

    Send(to_send, messages_);
}

std::optional<Message> Raft::HandleReadyReadIndex(const Message& req, uint64_t index) {
    if (req.from() == INVALID_ID || req.from() == id_) {
        ReadState rs;
        rs.index = index;
        rs.request_ctx = req.entries().at(0).data();
        read_states_.emplace_back(rs);
        return {};
    }

    Message m;
    m.set_to(req.from());
    m.set_msg_type(MsgReadIndexResp);
    m.set_index(index);
    *m.mutable_entries() = req.entries();
    return m;
}

Result<void> Raft::StepCandidate(const Message& m) {
    switch (m.msg_type()) {
        case MsgPropose:
            return RaftError(RaftErrorCode::ProposalDropped);

        case MsgAppend:
            BecomeFollower(m.term(), m.from());
            HandleAppendEntries(m);
            break;

        case MsgHeartbeat:
            BecomeFollower(m.term(), m.from());
            HandleHeartbeat(m);
            break;

        case MsgSnapshot:
            BecomeFollower(m.term(), m.from());
            HandleSnapshot(m);
            break;

        case MsgRequestPreVoteResponse:
        case MsgRequestVoteResponse:
            // Only handle vote responses corresponding to our candidacy (while in
            // state Candidate, we may get stale MsgPreVoteResp messages in this term from
            // our pre-candidate state).
            if ((state_ == StateRole::PreCandidate && m.msg_type() != MsgRequestPreVoteResponse) ||
                (state_ == StateRole::Candidate && m.msg_type() != MsgRequestVoteResponse)) {
                return {};
            }
            std::ignore = Poll(m.from(), m.msg_type(), !m.reject());
            MaybeCommitByVote(m);
            break;

        case MsgTimeoutNow:
            SPDLOG_DEBUG("ignored MsgTimeoutNow, term={}, from={}", m.term(), m.from());
            break;

        case MsgReadIndex:
            SPDLOG_INFO("no leader at term={}; dropping read index msg", m.term());
            break;

        default:
            break;
    }

    return {};
}

Result<void> Raft::StepFollower(Message& m) {
    switch (m.msg_type()) {
        case MsgPropose:
            if (leader_id_ == INVALID_ID) {
                return RaftError(RaftErrorCode::ProposalDropped);
            }
            if (disable_proposal_forwarding_) {
                return RaftError(RaftErrorCode::ProposalDropped);
            }
            m.set_to(leader_id_);
            Send(m, messages_);
            break;

        case MsgAppend:
            election_elapsed_ = 0;
            leader_id_ = m.from();
            HandleAppendEntries(m);
            break;

        case MsgHeartbeat:
            election_elapsed_ = 0;
            leader_id_ = m.from();
            HandleHeartbeat(m);
            break;

        case MsgSnapshot:
            election_elapsed_ = 0;
            leader_id_ = m.from();
            HandleSnapshot(m);
            break;

        case MsgTransferLeader:
            if (leader_id_ == INVALID_ID) {
                SPDLOG_INFO("no leader at term {}; dropping leader transfer msg", term_);
                return {};
            }
            m.set_to(leader_id_);
            Send(m, messages_);
            break;

        case MsgTimeoutNow:
            if (promotable_) {
                Hup(true);
            } else {
                SPDLOG_INFO("received MsgTimeoutNow from {} but is not promotable", m.from());
            }
            break;

        case MsgReadIndex:
            if (leader_id_ == INVALID_ID) {
                SPDLOG_INFO("no leader at term {}; dropping read index msg", term_);
                return {};
            }
            m.set_to(leader_id_);
            Send(m, messages_);
            break;

        case MsgReadIndexResp: {
            if (m.entries_size() != 1) {
                SPDLOG_ERROR(
                    "invalid format of MsgReadIndexResp from {}, entries_size={}", m.from(),
                    m.entries_size()
                );
                return {};
            }

            ReadState rs;
            rs.index = m.index();
            rs.request_ctx = m.entries().at(0).data();

            read_states_.emplace_back(rs);
            std::ignore = raft_log_.MaybeCommit(m.index(), m.term());
            break;
        }

        default:
            break;
    }

    return {};
}

bool Raft::CheckQuorumActive() {
    return progress_tracker_.QuorumRecentlyActive(id_);
}

bool Raft::CommitToCurrentTerm() const {
    const auto term_result = raft_log_.Term(raft_log_.committed());
    return term_result && *term_result == term_;
}

void Raft::HandleHeartbeatResponse(const Message& m) {
    Progress* p;
    if (p = progress_tracker_.get(m.from()); p == nullptr) {
        SPDLOG_INFO("no progress available for {}", m.from());
        return;
    }
    Progress& pr = *p;

    // update followers committed index via heartbeat response
    pr.UpdateCommitted(m.commit());
    pr.recent_active() = true;
    SPDLOG_DEBUG(
        "HandleHeartbeatResponse: from={}, index={}, reject={}", m.from(), m.index(), m.reject()
    );
    pr.Resume();

    if (pr.state() == ProgressState::Replicate && pr.inflights().Full()) {
        pr.inflights().FreeFirstOne();
    }

    // Does it request snapshot?
    SPDLOG_DEBUG(
        "HandleHeartbeatResp check: from={}, matched={}, next={}, last_index={}", m.from(),
        pr.matched(), pr.next_idx(), raft_log_.LastIndex()
    );
    if (pr.matched() < raft_log_.LastIndex() || pr.pending_request_snapshot() != INVALID_INDEX) {
        SPDLOG_DEBUG("HandleHeartbeatResp: sending append to {}", m.from());
        RaftCore::SendAppend(m.from(), pr, messages_);
    }

    if (read_only_.option() != ReadOnlyOption::Safe || m.context().empty()) {
        return;
    }

    if (auto acks = read_only_.RecvACK(m.from(), m.context()); !acks) {
        return;
    } else {
        // FIXME(fanyang)
        if (progress_tracker_.HasQuorum(*acks)) {}
    }

    for (const auto rs : read_only_.Advance(m.context())) {
        if (auto r = HandleReadyReadIndex(rs.req, rs.index)) {
            Send(*r, messages_);
        }
    }
}

void Raft::HandleSnapshotStatus(const Message& m) {
    Progress* p = progress_tracker_.get(m.from());
    if (p == nullptr) {
        return;
    }
    Progress& pr = *p;

    if (pr.state() != ProgressState::Snapshot) {
        return;
    }

    if (m.reject()) {
        SPDLOG_DEBUG(
            "HandleSnapshotStatus: reject from {}, index={}, reject_hint={}, log_term={}", m.from(),
            m.index(), m.reject_hint(), m.log_term()
        );
        pr.SnapshotFailure();
        pr.BecomeProbe();
    } else {
        pr.BecomeProbe();
    }

    pr.Pause();
    pr.pending_request_snapshot() = INVALID_INDEX;
}

void Raft::HandleUnreachable(const Message& m) {
    Progress* p = progress_tracker_.get(m.from());
    if (p == nullptr) {
        return;
    }
    Progress& pr = *p;

    if (pr.state() == ProgressState::Replicate) {
        pr.BecomeProbe();
    }

    SPDLOG_INFO("failed to send message to {} because it is unreachable", m.from());
}

void Raft::HandleTransferLeader(const Message& m) {
    const uint64_t from = m.from();
    Progress* p = progress_tracker_.get(from);
    if (p == nullptr) {
        return;
    }
    Progress& pr = *p;

    if (progress_tracker_.conf().learners.contains(from)) {
        return;
    }

    auto lead_transferee = from;
    if (const auto last_lead_transferee = lead_transferee_) {
        if (*last_lead_transferee == lead_transferee) {
            return;
        }
        AbortLeaderTransfer();
    }

    if (lead_transferee == id_) {
        return;
    }

    election_elapsed_ = 0;
    lead_transferee_ = lead_transferee;

    if (pr.matched() == raft_log_.LastIndex()) {
        SendTimeoutNow(lead_transferee);
    } else {
        RaftCore::SendAppend(lead_transferee, pr, messages_);
    }
}

void Raft::BroadcastHeartbeat() {
    const auto& ctx = read_only_.LastPendingRequestCtx();
    BroadcastHeartbeat(ctx);
}

void Raft::SendHeartbeat(
    const uint64_t to, const Progress& pr, const std::optional<std::string>& ctx,
    std::vector<Message>& messages
) {
    Message m;
    m.set_to(to);
    m.set_msg_type(MsgHeartbeat);
    m.set_commit(std::min(pr.matched(), raft_log_.committed()));
    if (ctx) {
        std::string s(ctx->begin(), ctx->end());
        m.set_context(s);
    }
    Send(m, messages);
}

void Raft::BroadcastHeartbeat(const std::optional<std::string>& ctx) {
    for (const auto& [id, pr] : progress_tracker_.progress_map()) {
        if (id == id_) {
            continue;
        }
        SendHeartbeat(id, pr, ctx, messages_);
    }
}

Result<void> Raft::StepLeader(const Message& m) {
    switch (m.msg_type()) {
        case MsgBeat:
            BroadcastHeartbeat();
            return {};

        case MsgCheckQuorum:
            if (!CheckQuorumActive()) {
                SPDLOG_WARN("stepped down to follower since quorum is not active");
                BecomeFollower(term_, INVALID_ID);
            }
            return {};

        case MsgPropose:
            if (m.entries_size() == 0) {
                PANIC("stepped empty MsgProp");
            }

            if (!progress_tracker_.progress_map().contains(id_)) {
                return RaftError(RaftErrorCode::ProposalDropped);
            }

            if (lead_transferee_) {
                return RaftError(RaftErrorCode::ProposalDropped);
            }

            for (size_t i = 0; i < m.entries().size(); i++) {
                auto& ent = m.entries().at(i);
                ConfChangeV2 cc;
                if (ent.entry_type() == EntryConfChangeV2) {
                    if (!cc.ParseFromString(ent.data())) {
                        return RaftError(RaftErrorCode::ProposalDropped);
                    }
                }
            }

            {
                std::vector<Entry> entries(m.entries().begin(), m.entries().end());
                if (!AppendEntry(entries)) {
                    return RaftError(RaftErrorCode::ProposalDropped);
                }
            }
            BroadcastAppend();
            return {};

        case MsgReadIndex: {
            if (!CommitToCurrentTerm()) {
                // Reject read only request when this leader has not committed any log entry
                // in its term.
                SPDLOG_INFO("leader has not yet committed in its term; dropping read index msg");
                return {};
            }

            if (progress_tracker_.IsSingleton()) {
                const auto read_index = raft_log_.committed();
                if (auto resp = HandleReadyReadIndex(m, read_index)) {
                    Send(*resp, messages_);
                }
                return {};
            }

            switch (read_only_.option()) {
                case ReadOnlyOption::Safe: {
                    const std::string ctx = m.entries(0).data();
                    read_only_.AddRequest(raft_log_.committed(), m, id_);
                    BroadcastHeartbeat(ctx);
                    break;
                }
                case ReadOnlyOption::LeaseBased: {
                    const auto read_index = raft_log_.committed();
                    if (auto resp = HandleReadyReadIndex(m, read_index)) {
                        Send(*resp, messages_);
                    }
                    break;
                }
            }
            return {};
        }

        default:
            break;
    }

    switch (m.msg_type()) {
        case MsgAppendResponse:
            SPDLOG_DEBUG("StepLeader: received MsgAppendResponse from {}", m.from());
            HandleAppendResponse(m);
            break;
        case MsgHeartbeatResponse:
            HandleHeartbeatResponse(m);
            break;
        case MsgSnapStatus:
            HandleSnapshotStatus(m);
            break;
        case MsgUnreachable:
            HandleUnreachable(m);
            break;
        case MsgTransferLeader:
            HandleTransferLeader(m);
            break;
        default:
            if (progress_tracker_.get(m.from()) == nullptr) {
                SPDLOG_DEBUG("no progress available for {}", m.from());
            }
    }

    return {};
}

void Raft::SendAppend(const uint64_t to) {
    auto& pr = progress_tracker_.at(to);
    RaftCore::SendAppend(to, pr, messages_);
}

void Raft::SendAppendAggressively(const uint64_t to) {
    auto& pr = progress_tracker_.at(to);
    RaftCore::SendAppendAggressively(to, pr, messages_);
}

Result<void> Raft::Step(Message& m) {
    if (m.term() == 0) {
        // local message - fall through to process based on current state
    } else if (m.term() > term_) {
        if (m.msg_type() == MsgRequestVote || m.msg_type() == MsgRequestPreVote) {
            const bool force = m.context() == CAMPAIGN_TRANSFER;
            const bool in_lease =
                check_quorum_ && leader_id_ != INVALID_ID && election_elapsed_ < election_timeout_;

            if (!force && in_lease) {
                SPDLOG_INFO("ignored vote from {}: lease is not expired");
                return {};
            }
        }

        if (m.msg_type() == MsgRequestPreVote ||
            (m.msg_type() == MsgRequestPreVoteResponse && !m.reject())) {
            // For a pre-vote request:
            // Never change our term in response to a pre-vote request.
        } else {
            SPDLOG_INFO("received a message with higher term from {}", m.from());
            if (m.msg_type() == MsgAppend || m.msg_type() == MsgHeartbeat ||
                m.msg_type() == MsgSnapshot) {
                BecomeFollower(m.term(), m.from());
            } else {
                BecomeFollower(m.term(), INVALID_ID);
            }
        }
        // Fall through to process the message
    } else if (m.term() < term_) {
        if ((check_quorum_ || pre_vote_) &&
            (m.msg_type() == MsgHeartbeat || m.msg_type() == MsgAppend)) {
            Message to_send;
            to_send.set_to(m.from());
            to_send.set_msg_type(MsgAppendResponse);
            Send(to_send, messages_);
        } else if (m.msg_type() == MsgRequestPreVote) {
            Message to_send;
            to_send.set_to(m.from());
            to_send.set_msg_type(MsgRequestPreVoteResponse);
            to_send.set_reject(true);
            to_send.set_term(term_);
            Send(to_send, messages_);
        } else {
            // ignore other cases
            SPDLOG_INFO("ignored a message with lower term, from={}", m.from());
        }
        return {};
    }

    // m.term() == term_
    switch (m.msg_type()) {
        case MsgHup:
            Hup(false);
            return {};

        case MsgRequestVote:
        case MsgRequestPreVote: {
            const bool can_vote = (vote_ == m.from()) ||
                (vote_ == INVALID_ID && leader_id_ == INVALID_ID) ||
                (m.msg_type() == MsgRequestPreVote && m.term() > term_);

            if (can_vote && raft_log_.IsUpToDate(m.index(), m.log_term()) &&
                (m.index() > raft_log_.LastIndex() || priority_ <= m.priority())) {
                Message to_send;
                to_send.set_to(m.from());
                to_send.set_msg_type(VoteRespMsgType(m.msg_type()));
                to_send.set_reject(false);
                to_send.set_term(m.term());
                Send(to_send, messages_);

                if (m.msg_type() == MsgRequestVote) {
                    // Only record real votes.
                    election_elapsed_ = 0;
                    vote_ = m.from();
                }
            } else {
                Message to_send;
                to_send.set_to(m.from());
                to_send.set_msg_type(VoteRespMsgType(m.msg_type()));
                to_send.set_reject(true);
                to_send.set_term(term_);

                const auto [commit, commit_term] = raft_log_.CommitInfo();
                to_send.set_commit(commit);
                to_send.set_commit_term(commit_term);
                Send(to_send, messages_);

                MaybeCommitByVote(m);
            }
            break;
        }

        default:
            switch (state_) {
                case StateRole::PreCandidate:
                case StateRole::Candidate:
                    return StepCandidate(m);

                case StateRole::Follower:
                    return StepFollower(m);

                case StateRole::Leader:
                    return StepLeader(m);
            }
    }

    return {};
}

void Raft::HandleAppendEntries(const Message& m) {
    if (pending_request_snapshot_ != INVALID_INDEX) {
        SendRequestSnapshot();
        return;
    }

    if (m.index() < raft_log_.committed()) {
        Message to_send;
        to_send.set_to(m.from());
        to_send.set_msg_type(MsgAppendResponse);
        to_send.set_index(raft_log_.committed());
        to_send.set_commit(raft_log_.committed());
        Send(to_send, messages_);
        return;
    }

    Message to_send;
    to_send.set_to(m.from());
    to_send.set_msg_type(MsgAppendResponse);

    SPDLOG_INFO(
        "HandleAppendEntries: index={}, log_term={}, commit={}, num_entries={}", m.index(),
        m.log_term(), m.commit(), m.entries_size()
    );
    const auto r = raft_log_.MaybeAppend(
        m.index(), m.log_term(), m.commit(), {m.entries().begin(), m.entries().end()}
    );
    if (!r) {
        // Fatal error (e.g., conflict with committed entry)
        PANIC("MaybeAppend returned error: {}", r.error());
    } else if (r->term_matched) {
        to_send.set_index(r->last_index);
    } else {
        const auto [hint_index, hint_term] =
            raft_log_.FindConflictByTerm(std::min(m.index(), raft_log_.LastIndex()), m.log_term());

        if (!hint_term.has_value()) {
            PANIC("term({}) must be valid", hint_index);
        }

        to_send.set_index(m.index());
        to_send.set_reject(true);
        to_send.set_reject_hint(hint_index);
        to_send.set_log_term(*hint_term);
    }

    to_send.set_commit(raft_log_.committed());
    Send(to_send, messages_);
}

bool Raft::TickElection() {
    heartbeat_elapsed_ += 1;
    election_elapsed_ += 1;

    bool has_ready = false;
    if (election_elapsed_ >= randomized_election_timeout_) {
        election_elapsed_ = 0;
        Message m;
        m.set_to(INVALID_ID);
        m.set_msg_type(MsgHup);
        m.set_from(id_);
        has_ready = true;
        std::ignore = Step(m);
    }

    if (state_ != StateRole::Leader) {
        return has_ready;
    }

    if (heartbeat_elapsed_ >= heartbeat_timeout_) {
        heartbeat_elapsed_ = 0;
        has_ready = true;
        Message m;
        m.set_to(INVALID_ID);
        m.set_msg_type(MsgBeat);
        m.set_from(id_);
        std::ignore = Step(m);
    }

    return has_ready;
}

bool Raft::TickHeartbeat() {
    heartbeat_elapsed_ += 1;
    election_elapsed_ += 1;

    bool has_ready = false;
    if (election_elapsed_ >= randomized_election_timeout_) {
        election_elapsed_ = 0;
        if (check_quorum_) {
            Message m;
            m.set_to(INVALID_ID);
            m.set_msg_type(MsgCheckQuorum);
            m.set_from(id_);
            has_ready = true;
            std::ignore = Step(m);
        }
        if (state_ == StateRole::Leader && lead_transferee_) {
            AbortLeaderTransfer();
        }
    }

    if (state_ != StateRole::Leader) {
        return has_ready;
    }

    if (heartbeat_elapsed_ >= heartbeat_timeout_) {
        heartbeat_elapsed_ = 0;
        has_ready = true;
        Message m;
        m.set_to(INVALID_ID);
        m.set_msg_type(MsgBeat);
        m.set_from(id_);
        std::ignore = Step(m);
    }

    return has_ready;
}

bool Raft::Tick() {
    switch (state_) {
        case StateRole::Follower:
        case StateRole::Candidate:
        case StateRole::PreCandidate:
            return TickElection();
        case StateRole::Leader:
            return TickHeartbeat();
        default:
            PANIC("unexpected state");
    }
}

void Raft::SetPriority(const uint64_t priority) {
    priority_ = priority;
}

void Raft::ReduceUncommittedSize(const std::vector<Entry>& ents) {
    if (state_ != StateRole::Leader) {
        return;
    }

    if (!uncommitted_state_.MaybeReduceUncommittedSize(ents)) {
        SPDLOG_WARN(
            "try to reduce uncommitted size less than 0, first index of pending ents is {}",
            ents.front().index()
        );
    }
}

void Raft::CommitApply(const uint64_t applied) {
    CommitApplyInternal(applied, false);
}

Result<void> Raft::RequestSnapshot() {
    if (state_ == StateRole::Leader) {
        SPDLOG_INFO("can not request snapshot on leader; dropping request snapshot");
    } else if (leader_id_ == INVALID_ID) {
        SPDLOG_INFO("no leader; dropping request snapshot, term={}", term_);
    } else if (snapshot().has_value() || pending_request_snapshot_ != INVALID_INDEX) {
        SPDLOG_INFO("there is a pending snapshot; dropping request snapshot");
    } else {
        const auto request_index = raft_log_.LastIndex();
        const auto request_index_term = Unwrap(raft_log_.Term(request_index));
        if (term_ == request_index_term) {
            pending_request_snapshot_ = request_index;
            SendRequestSnapshot();
            return {};
        }
        SPDLOG_INFO(
            "mismatched term; dropping request snapshot, term={}, last_term={}", term_,
            request_index_term
        );
    }
    return RaftError(RaftErrorCode::RequestSnapshotDropped);
}

ProgressTracker& Raft::progress_tracker() {
    return progress_tracker_;
}

const ProgressTracker& Raft::progress_tracker() const {
    return progress_tracker_;
}

HardState Raft::hard_state() const {
    HardState hs;
    hs.set_term(term_);
    hs.set_vote(vote_);
    hs.set_commit(raft_log_.committed());
    return hs;
}

SoftState Raft::soft_state() const {
    SoftState ss{};
    ss.leader_id = leader_id_;
    ss.raft_state = state_;
    return ss;
}

const std::vector<ReadState>& Raft::read_states() const {
    return read_states_;
}

std::vector<ReadState>& Raft::read_states() {
    return read_states_;
}

uint64_t Raft::id() const {
    return id_;
}

uint64_t Raft::term() const {
    return term_;
}

StateRole Raft::state() const {
    return state_;
}

const RaftLog& Raft::raft_log() const {
    return raft_log_;
}

RaftLog& Raft::raft_log() {
    return raft_log_;
}

uint64_t Raft::max_committed_size_per_ready() const {
    return max_committed_size_per_ready_;
}

uint64_t& Raft::max_committed_size_per_ready() {
    return max_committed_size_per_ready_;
}

const std::vector<Message>& Raft::messages() const {
    return messages_;
}

std::vector<Message>& Raft::messages() {
    return messages_;
}

std::optional<std::reference_wrapper<Snapshot>> Raft::snapshot() {
    return raft_log_.unstable().snapshot();
}

const std::optional<Snapshot>& Raft::snapshot() const {
    return raft_log_.unstable().snapshot();
}

void Raft::Ping() {
    if (state_ == StateRole::Leader) {
        BroadcastHeartbeat();
    }
}

bool LeaveJoint(const ConfChangeV2& cc) {
    return cc.transition() == Auto && cc.changes().empty();
}

std::optional<bool> EnterJoint(const ConfChangeV2& cc) {
    if (cc.transition() != Auto || cc.changes_size() > 1) {
        switch (cc.transition()) {
            case Auto:
            case Implicit:
                return true;
            case Explicit:
                return false;
            default:
                PANIC("unexpected transition");
        }
    }
    return {};
}

Result<ConfState> Raft::ApplyConfChange(const ConfChangeV2& cc) {
    ConfChanger changer(progress_tracker_);

    Result<std::pair<TrackerConfiguration, MapChange>> r;
    if (LeaveJoint(cc)) {
        SPDLOG_INFO("ApplyConfChange: LeaveJoint");
        r = changer.LeaveJoint();
    } else {
        std::vector ccs(cc.changes().begin(), cc.changes().end());
        if (const auto auto_leave = EnterJoint(cc)) {
            SPDLOG_INFO("ApplyConfChange: EnterJoint, auto_leave={}", *auto_leave);
            r = changer.EnterJoint(*auto_leave, ccs);
        } else {
            SPDLOG_INFO("ApplyConfChange: Simple, num_changes={}", ccs.size());
            r = changer.Simple(ccs);
        }
    }

    if (r) {
        const auto& cfg = r->first;
        const auto& changes = r->second;
        SPDLOG_INFO("ApplyConfChange: success, num_changes={}", changes.size());
        for (const auto& [id, change_type] : changes) {
            SPDLOG_INFO(
                "  change: id={}, type={}", id, change_type == MapChangeType::Add ? "Add" : "Remove"
            );
        }
        progress_tracker_.ApplyConf(cfg, changes, raft_log_.LastIndex());
    } else {
        SPDLOG_ERROR("ApplyConfChange: failed, error={}", r.error().ToString());
    }

    return PostConfChange();
}

MessageType Raft::VoteRespMsgType(const MessageType mt) {
    switch (mt) {
        case MsgRequestVote:
            return MsgRequestVoteResponse;
        case MsgRequestPreVote:
            return MsgRequestPreVoteResponse;
        default:
            PANIC("not a vote message: {}", MessageType_Name(mt));
    }
}

void Raft::ResetRandomizedElectionTimeout() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    // Range is [min_election_timeout, max_election_timeout - 1] to match raft-rs
    std::uniform_int_distribution dist(min_election_timeout_, max_election_timeout_ - 1);
    const size_t timeout = dist(gen);
    size_t prev_timeout = randomized_election_timeout_;
    randomized_election_timeout_ = timeout;
    SPDLOG_INFO("reset election timeout, {} -> {}", prev_timeout, timeout);
}

void Raft::AbortLeaderTransfer() {
    lead_transferee_ = {};
}

void Raft::Reset(const uint64_t term) {
    if (term_ != term) {
        term_ = term;
        vote_ = INVALID_ID;
    }
    leader_id_ = INVALID_ID;
    ResetRandomizedElectionTimeout();
    election_elapsed_ = 0;
    heartbeat_elapsed_ = 0;

    AbortLeaderTransfer();
    progress_tracker_.ResetVotes();

    pending_conf_index_ = 0;
    read_only_ = ReadOnly(read_only_.option());
    pending_request_snapshot_ = INVALID_INDEX;

    const uint64_t last_index = raft_log_.LastIndex();
    const uint64_t committed = raft_log_.committed();
    const uint64_t persisted = raft_log_.persisted();
    const uint64_t self_id = id_;
    for (auto& [id, pr] : progress_tracker_.progress_map()) {
        pr.Reset(last_index + 1);
        if (id == self_id) {
            pr.matched() = persisted;
            pr.committed_index() = committed;
        }
    }
}

void Raft::BecomeFollower(const uint64_t term, const uint64_t leader_id) {
    const uint64_t pending_request_snapshot = pending_request_snapshot_;
    Reset(term);
    leader_id_ = leader_id;
    const auto from_role = state_;
    state_ = StateRole::Follower;
    pending_request_snapshot_ = pending_request_snapshot;
    raft_log_.max_apply_unpersisted_log_limit() = 0;

    SPDLOG_INFO("became follower, term={}, from_role={}", term, format_as(from_role));
}

size_t Raft::max_inflight_messages() const {
    return max_inflight_;
}

size_t Raft::inflight_buffers_size() const {
    size_t total = 0;
    for (const auto& [id, pr] : progress_tracker_.progress_map()) {
        if (pr.inflights().buffer_is_allocated()) {
            total += pr.inflights().BufferSize() * sizeof(uint64_t);
        }
    }
    return total;
}

void Raft::maybe_free_inflight_buffers() {
    for (auto& [id, pr] : progress_tracker_.progress_map()) {
        if (pr.inflights().Count() == 0 && pr.inflights().buffer_is_allocated()) {
            // Free the buffer if this peer has no inflight messages
            pr.inflights().Reset();
        }
    }
}

void Raft::adjust_max_inflight_msgs(uint64_t id, size_t max_inflight) {
    auto* pr = progress_tracker_.get(id);
    if (pr != nullptr) {
        pr->inflights().SetCapacity(max_inflight);
    }
}

bool Raft::ConfStatesEqualIgnoringOrder(const ConfState& a, const ConfState& b) {
    // Compare voters
    if (a.voters().size() != b.voters().size()) {
        return false;
    }
    std::vector<uint64_t> a_voters(a.voters().begin(), a.voters().end());
    std::vector<uint64_t> b_voters(b.voters().begin(), b.voters().end());
    std::sort(a_voters.begin(), a_voters.end());
    std::sort(b_voters.begin(), b_voters.end());
    if (a_voters != b_voters) {
        return false;
    }

    // Compare learners
    if (a.learners().size() != b.learners().size()) {
        return false;
    }
    std::vector<uint64_t> a_learners(a.learners().begin(), a.learners().end());
    std::vector<uint64_t> b_learners(b.learners().begin(), b.learners().end());
    std::sort(a_learners.begin(), a_learners.end());
    std::sort(b_learners.begin(), b_learners.end());
    if (a_learners != b_learners) {
        return false;
    }

    // Voters outgoing and incoming for joint configs
    if (a.voters_outgoing().size() != b.voters_outgoing().size()) {
        return false;
    }
    std::vector<uint64_t> a_outgoing(a.voters_outgoing().begin(), a.voters_outgoing().end());
    std::vector<uint64_t> b_outgoing(b.voters_outgoing().begin(), b.voters_outgoing().end());
    std::sort(a_outgoing.begin(), a_outgoing.end());
    std::sort(b_outgoing.begin(), b_outgoing.end());
    if (a_outgoing != b_outgoing) {
        return false;
    }

    return true;
}

void Raft::EnableGroupCommit(bool enable) {
    progress_tracker_.EnableGroupCommit(enable);
    // When disabling group commit on leader, recalculate commit and broadcast
    if (state_ == StateRole::Leader && !enable && MaybeCommit()) {
        BroadcastAppend();
    }
}

bool Raft::GroupCommit() const {
    return progress_tracker_.GroupCommit();
}

void Raft::AssignCommitGroups(const std::vector<std::pair<uint64_t, uint64_t>>& ids) {
    for (const auto& [peer_id, group_id] : ids) {
        ASSERT(group_id > 0, "group_id must be > 0");
        if (auto* pr = progress_tracker_.get(peer_id)) {
            pr->SetCommitGroupID(group_id);
        }
    }
    // If leader with group commit enabled, try to commit and broadcast
    if (state_ == StateRole::Leader && GroupCommit() && MaybeCommit()) {
        BroadcastAppend();
    }
}

std::optional<bool> Raft::CheckGroupCommitConsistent() {
    if (state_ != StateRole::Leader) {
        return std::nullopt;
    }
    // Need to wait for current term's entry to be applied
    const auto term_result = raft_log_.Term(raft_log_.applied());
    if (!term_result || *term_result != term_) {
        return std::nullopt;
    }
    auto [index, use_group_commit] = progress_tracker_.MaxCommittedIndex();
    return use_group_commit && index == raft_log_.committed();
}

}  // namespace raftpp
