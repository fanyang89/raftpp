#include "raftpp/core/raft.h"

#include <random>
#include <ranges>

#include "raftpp/core/conf_changer.h"
#include "raftpp/core/conf_restore.h"
#include "raftpp/core/util.h"
#include "raftpp/logging.h"

namespace raftpp {

constexpr std::string_view kCampaignPreElection = "CampaignPreElection";
constexpr std::string_view kCampaignElection = "CampaignElection";
constexpr std::string_view kCampaignTransfer = "CampaignTransfer";

bool UncommittedState::IsNoLimit() const {
    return max_uncommitted_size == std::numeric_limits<size_t>::max();
}

bool UncommittedState::MaybeIncreaseUncommittedSize(std::span<const Entry> entries) {
    if (IsNoLimit()) {
        return true;
    }

    const std::size_t size = std::transform_reduce(
        entries.begin(), entries.end(), std::size_t{0}, std::plus{},
        [](const Entry& e) { return capnp_util::reader<msg::Entry>(e).getData().size(); }
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
            return capnp_util::reader<msg::Entry>(e).getIndex() <= last_log_tail_index;
        }) | std::views::transform([](const Entry& e) {
            return capnp_util::reader<msg::Entry>(e).getData().size();
        }),
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
        PANIC("invalid restore: configuration mismatch");
    }

    // Only load hard state if it's explicitly configured (for production use)
    // In tests, we start with default state to match raft-rs behavior
    auto default_hs = capnp_util::make<msg::HardState>();
    if (config.load_state_on_startup &&
        !capnp_util::equal<msg::HardState>(
            capnp_util::reader<msg::HardState>(raft_state.hard_state),
            capnp_util::reader<msg::HardState>(default_hs)
        )) {
        LoadState(raft_state.hard_state);
    }

    if (config.applied > 0) {
        CommitApplyInternal(config.applied, true);
    }

    BecomeFollower(term_, kInvalidId);

    RaftLog& log = raft_log_;
    RAFTPP_LOG_INFO(
        "new raft instance, term={}, commit={}, applied={}, last_index={}, last_term={}, peers={}",
        term_, log.committed(), log.applied(), log.LastIndex(), log.LastTerm(),
        fmt::format("{}", progress_tracker_.conf().voters)
    );
}

ConfState Raft::PostConfChange() {
    RAFTPP_LOG_INFO("switched to configuration, config={}", progress_tracker_.conf());
    auto cs = progress_tracker_.conf().ToConfState();
    const bool is_voter = progress_tracker_.conf().voters.Contains(id_);
    promotable_ = is_voter;

    if (!is_voter && state_ == StateRole::Leader) {
        return cs;
    }

    auto cs_reader = capnp_util::reader<msg::ConfState>(cs);
    if (state_ != StateRole::Leader || cs_reader.getVoters().size() == 0) {
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
    auto hs_reader = capnp_util::reader<msg::HardState>(hs);
    if (hs_reader.getCommit() < raft_log_.committed() ||
        hs_reader.getCommit() > raft_log_.LastIndex()) {
        PANIC(
            "hs.commit {} is out of range [{}, {}]", hs_reader.getCommit(), raft_log_.committed(),
            raft_log_.LastIndex()
        );
    }
    raft_log_.committed() = hs_reader.getCommit();
    term_ = hs_reader.getTerm();
    vote_ = hs_reader.getVote();
}

bool Raft::MaybeIncreaseUncommittedSize(const std::span<const Entry> entries) {
    return uncommitted_state_.MaybeIncreaseUncommittedSize(entries);
}

bool Raft::AppendEntry(const Entry& entry) {
    std::vector<Entry> vec;
    vec.push_back(CloneEntry(entry));
    return AppendEntry(std::move(vec));
}

bool Raft::AppendEntry(std::vector<Entry> entries) {
    if (!MaybeIncreaseUncommittedSize(entries)) {
        return false;
    }

    const uint64_t last_index = raft_log_.LastIndex();
    for (size_t i = 0; i < entries.size(); ++i) {
        auto& entry = entries[i];
        auto builder = capnp_util::builder<msg::Entry>(entry);
        builder.setTerm(term_);
        builder.setIndex(last_index + i + 1);
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
            RAFTPP_LOG_ERROR(
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
    leader_id_ = kInvalidId;
    RAFTPP_LOG_INFO("became pre-candidate, term={}", term_);
}

void Raft::BecomeCandidate() {
    ASSERT(state_ != StateRole::Leader, "invalid transition [leader -> candidate]");
    const auto term = term_ + 1;
    Reset(term);
    const auto id = id_;
    vote_ = id;
    state_ = StateRole::Candidate;
    promotable_ = progress_tracker_.conf().voters.Contains(id);
    RAFTPP_LOG_INFO("became candidate, term={}", term_);
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

    if (!AppendEntry(capnp_util::make<msg::Entry>())) {
        PANIC("appending an empty entry should never be dropped");
    }

    RAFTPP_LOG_INFO("became leader at term {}", term_);
}

VoteResult Raft::Poll(const uint64_t from, MessageType mt, const bool vote) {
    progress_tracker_.RecordVote(from, vote);
    const auto& r = progress_tracker_.CountVotes();
    if (from != id_) {
        RAFTPP_LOG_DEBUG("received votes response from {}, vote={}", from, vote);
    }

    switch (r.result) {
        case VoteResult::Pending:
            break;
        case VoteResult::Lost:
            BecomeFollower(term_, kInvalidId);
            break;
        case VoteResult::Won:
            if (state_ == StateRole::PreCandidate) {
                Campaign(kCampaignElection);
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

    if (campaign_type == kCampaignPreElection) {
        BecomePreCandidate();
        vote_msg = MessageType::MSG_REQUEST_PRE_VOTE;
        term = term_ + 1;
    } else {
        BecomeCandidate();
        vote_msg = MessageType::MSG_REQUEST_VOTE;
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

        auto m = capnp_util::make<msg::Message>();
        auto m_builder = capnp_util::builder<msg::Message>(m);
        m_builder.setTo(id);
        m_builder.setMsgType(vote_msg);
        m_builder.setTerm(term);
        m_builder.setIndex(raft_log_.LastIndex());
        m_builder.setLogTerm(raft_log_.LastTerm());
        m_builder.setCommit(commit);
        m_builder.setCommitTerm(commit_term);
        if (campaign_type == kCampaignTransfer) {
            m_builder.setContext(kj::arrayPtr(
                reinterpret_cast<const kj::byte*>(campaign_type.data()), campaign_type.size()
            ));
        }

        Send(m, messages_);
    }
}

void Raft::Hup(const bool transfer_leader) {
    if (state_ == StateRole::Leader) {
        RAFTPP_LOG_DEBUG("ignoring MsgHup because already leader");
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
        RAFTPP_LOG_WARN(
            "cannot campaign at term {} since there are still pending configuration changes to "
            "apply",
            term_
        );
        return;
    }

    RAFTPP_LOG_INFO("starting a new election, term={}", term_);
    if (transfer_leader) {
        Campaign(kCampaignTransfer);
    } else if (pre_vote_) {
        Campaign(kCampaignPreElection);
    } else {
        Campaign(kCampaignElection);
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
            if (capnp_util::reader<msg::Entry>(e).getEntryType() ==
                EntryType::ENTRY_CONF_CHANGE_V2) {
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
        // Create an empty ConfChangeV2 for leaving joint configuration
        auto leave_cc = capnp_util::make<msg::ConfChangeV2>();
        // The empty ConfChangeV2 signals leaving joint configuration
        std::string serialized = capnp_util::toString(leave_cc);

        auto ent = capnp_util::make<msg::Entry>();
        auto ent_builder = capnp_util::builder<msg::Entry>(ent);
        ent_builder.setEntryType(EntryType::ENTRY_CONF_CHANGE_V2);
        ent_builder.setData(
            kj::arrayPtr(reinterpret_cast<const kj::byte*>(serialized.data()), serialized.size())
        );
        if (!AppendEntry(ent)) {
            PANIC("appending an empty EntryConfChangeV2 should never be dropped");
        }

        pending_conf_index_ = raft_log_.LastIndex();
    }
}

void Raft::MaybeCommitByVote(const Message& m) {
    auto m_reader = capnp_util::reader<msg::Message>(m);
    if (m_reader.getCommit() == 0 || m_reader.getCommitTerm() == 0) {
        return;
    }

    const uint64_t last_commit = raft_log_.committed();
    if (m_reader.getCommit() <= last_commit || state_ == StateRole::Leader) {
        return;
    }
    if (!raft_log_.MaybeCommit(m_reader.getCommit(), m_reader.getCommitTerm())) {
        return;
    }

    const auto& log = raft_log_;
    RAFTPP_LOG_INFO(
        "[commit: {}, last_index: {}, last_term: {}] fast-forwarded commit to vote request [index: "
        "{}, term: {}]",
        log.committed(), log.LastIndex(), log.LastTerm(), m_reader.getCommit(),
        m_reader.getCommitTerm()
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
        BecomeFollower(term, kInvalidId);
    }
}

void Raft::SendTimeoutNow(const uint64_t to) {
    auto m = capnp_util::make<msg::Message>();
    auto m_builder = capnp_util::builder<msg::Message>(m);
    m_builder.setTo(to);
    m_builder.setMsgType(MessageType::MSG_TIMEOUT_NOW);
    Send(m, messages_);
}

void Raft::HandleAppendResponse(const Message& m) {
    auto m_reader = capnp_util::reader<msg::Message>(m);
    auto next_probe_index = m_reader.getRejectHint();
    // pull out find_conflict_by_term for immutable borrow
    if (m_reader.getReject() && m_reader.getLogTerm() > 0) {
        next_probe_index =
            raft_log_.FindConflictByTerm(m_reader.getRejectHint(), m_reader.getLogTerm()).first;
    }

    auto* p = progress_tracker_.get(m_reader.getFrom());
    if (p == nullptr) {
        RAFTPP_LOG_WARN("no progress available for {}", m_reader.getFrom());
        return;
    }

    Progress& pr = *p;
    pr.recent_active() = true;
    RAFTPP_LOG_DEBUG(
        "HandleAppendResponse: from={}, index={}, reject={}", m_reader.getFrom(),
        m_reader.getIndex(), m_reader.getReject()
    );
    pr.UpdateCommitted(m_reader.getCommit());

    if (m_reader.getReject()) {
        RAFTPP_LOG_DEBUG(
            "HandleAppendResponse: reject from {}, index={}, reject_hint={}, log_term={}",
            m_reader.getFrom(), m_reader.getIndex(), m_reader.getRejectHint(), m_reader.getLogTerm()
        );
        if (pr.MaybeDecTo(m_reader.getIndex(), next_probe_index, m_reader.getRequestSnapshot())) {
            if (pr.state() == ProgressState::Replicate) {
                pr.BecomeProbe();
            }
            SendAppend(m_reader.getFrom());
        }
        return;
    }

    auto old_paused = pr.IsPaused();
    if (!pr.MaybeUpdate(m_reader.getIndex())) {
        return;
    }

    switch (pr.state()) {
        case ProgressState::Probe:
            pr.BecomeReplicate();
            break;
        case ProgressState::Replicate:
            pr.inflights().FreeTo(m_reader.getIndex());
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
        SendAppend(m_reader.getFrom());
    }

    SendAppendAggressively(m_reader.getFrom());

    if (m_reader.getFrom() == lead_transferee_) {
        if (progress_tracker_.at(m_reader.getFrom()).matched() == raft_log_.LastIndex()) {
            RAFTPP_LOG_INFO(
                "sent MsgTimeoutNow to {} after received MsgAppResp", m_reader.getFrom()
            );
            SendTimeoutNow(m_reader.getFrom());
        }
    }
}

void Raft::SendRequestSnapshot() {
    auto m = capnp_util::make<msg::Message>();
    auto m_builder = capnp_util::builder<msg::Message>(m);
    m_builder.setMsgType(MessageType::MSG_APPEND_RESPONSE);
    m_builder.setIndex(raft_log_.committed());
    m_builder.setReject(true);
    m_builder.setRejectHint(raft_log_.LastIndex());
    m_builder.setTo(leader_id_);
    m_builder.setRequestSnapshot(pending_request_snapshot_);

    const uint64_t reject_hint = raft_log_.LastIndex();
    if (const auto term = raft_log_.Term(reject_hint)) {
        m_builder.setLogTerm(*term);
    }

    Send(m, messages_);
}

void Raft::HandleHeartbeat(const Message& m) {
    auto m_reader = capnp_util::reader<msg::Message>(m);
    std::ignore = raft_log_.CommitTo(m_reader.getCommit());
    if (pending_request_snapshot_ != kInvalidIndex) {
        SendRequestSnapshot();
        return;
    }

    auto to_send = capnp_util::make<msg::Message>();
    auto to_send_builder = capnp_util::builder<msg::Message>(to_send);
    to_send_builder.setTo(m_reader.getFrom());
    to_send_builder.setMsgType(MessageType::MSG_HEARTBEAT_RESPONSE);
    to_send_builder.setContext(m_reader.getContext());
    to_send_builder.setCommit(raft_log_.committed());
    Send(to_send, messages_);
}

bool Raft::Restore(const Snapshot& snapshot) {
    auto snap_meta = capnp_util::reader<msg::Snapshot>(snapshot).getMetadata();
    if (snap_meta.getIndex() < raft_log_.committed()) {
        return false;
    }

    if (state_ != StateRole::Follower) {
        RAFTPP_LOG_WARN("non-follower attempted to restore snapshot, state={}", format_as(state_));
        BecomeFollower(term_ + 1, kInvalidId);
        return false;
    }

    // snap_meta is already defined earlier in the function
    const auto cs = snap_meta.getConfState();

    Set<uint64_t> cs_ids;
    for (const auto voter : cs.getVoters()) {
        cs_ids.insert(voter);
    }
    for (const auto voter : cs.getLearners()) {
        cs_ids.insert(voter);
    }
    for (const auto voter : cs.getVotersOutgoing()) {
        cs_ids.insert(voter);
    }
    if (!cs_ids.contains(id_)) {
        RAFTPP_LOG_WARN("attempted to restore snapshot but it is not in the ConfState");
        return false;
    }

    if (pending_request_snapshot_ == kInvalidIndex &&
        raft_log_.MatchTerm(snap_meta.getIndex(), snap_meta.getTerm())) {
        RAFTPP_LOG_INFO("fast-forwarded commit to snapshot");
        std::ignore = raft_log_.CommitTo(snap_meta.getIndex());
        return false;
    }

    std::ignore = raft_log_.Restore(snapshot);

    pending_request_snapshot_ = kInvalidIndex;

    RAFTPP_LOG_INFO("restored snapshot");
    return true;
}

void Raft::HandleSnapshot(const Message& m) {
    auto m_reader = capnp_util::reader<msg::Message>(m);
    auto to_send = capnp_util::make<msg::Message>();
    auto to_send_builder = capnp_util::builder<msg::Message>(to_send);
    to_send_builder.setMsgType(MessageType::MSG_APPEND_RESPONSE);
    to_send_builder.setTo(m_reader.getFrom());

    // Copy snapshot from message reader
    auto snap_reader = m_reader.getSnapshot();
    auto snapshot = capnp_util::make<msg::Snapshot>();
    auto snap_builder = capnp_util::builder<msg::Snapshot>(snapshot);
    snap_builder.setData(snap_reader.getData());
    snap_builder.setMetadata(snap_reader.getMetadata());

    if (Restore(snapshot)) {
        to_send_builder.setIndex(raft_log_.LastIndex());
    } else {
        to_send_builder.setIndex(raft_log_.committed());
    }

    Send(to_send, messages_);
}

std::optional<Message> Raft::HandleReadyReadIndex(const Message& req, uint64_t index) {
    auto req_reader = capnp_util::reader<msg::Message>(req);
    if (req_reader.getFrom() == kInvalidId || req_reader.getFrom() == id_) {
        ReadState rs;
        rs.index = index;
        auto entries = req_reader.getEntries();
        if (entries.size() > 0) {
            auto data = entries[0].getData();
            rs.request_ctx = std::string(reinterpret_cast<const char*>(data.begin()), data.size());
        }
        read_states_.emplace_back(rs);
        return {};
    }

    auto m = capnp_util::make<msg::Message>();
    auto m_builder = capnp_util::builder<msg::Message>(m);
    m_builder.setTo(req_reader.getFrom());
    m_builder.setMsgType(MessageType::MSG_READ_INDEX_RESP);
    m_builder.setIndex(index);

    // Copy entries from req to m
    auto req_entries = req_reader.getEntries();
    auto m_entries = m_builder.initEntries(req_entries.size());
    for (size_t i = 0; i < req_entries.size(); ++i) {
        m_entries.setWithCaveats(i, req_entries[i]);
    }

    return m;
}

Result<void> Raft::StepCandidate(const Message& m) {
    auto m_reader = capnp_util::reader<msg::Message>(m);
    switch (m_reader.getMsgType()) {
        case MessageType::MSG_PROPOSE:
            return RaftError(RaftErrorCode::ProposalDropped);

        case MessageType::MSG_APPEND:
            BecomeFollower(m_reader.getTerm(), m_reader.getFrom());
            HandleAppendEntries(m);
            break;

        case MessageType::MSG_HEARTBEAT:
            BecomeFollower(m_reader.getTerm(), m_reader.getFrom());
            HandleHeartbeat(m);
            break;

        case MessageType::MSG_SNAPSHOT:
            BecomeFollower(m_reader.getTerm(), m_reader.getFrom());
            HandleSnapshot(m);
            break;

        case MessageType::MSG_REQUEST_PRE_VOTE_RESPONSE:
        case MessageType::MSG_REQUEST_VOTE_RESPONSE:
            // Only handle vote responses corresponding to our candidacy (while in
            // state Candidate, we may get stale MsgPreVoteResp messages in this term from
            // our pre-candidate state).
            if ((state_ == StateRole::PreCandidate &&
                 m_reader.getMsgType() != MessageType::MSG_REQUEST_PRE_VOTE_RESPONSE) ||
                (state_ == StateRole::Candidate &&
                 m_reader.getMsgType() != MessageType::MSG_REQUEST_VOTE_RESPONSE)) {
                return {};
            }
            std::ignore = Poll(m_reader.getFrom(), m_reader.getMsgType(), !m_reader.getReject());
            MaybeCommitByVote(m);
            break;

        case MessageType::MSG_TIMEOUT_NOW:
            RAFTPP_LOG_DEBUG(
                "ignored MsgTimeoutNow, term={}, from={}", m_reader.getTerm(), m_reader.getFrom()
            );
            break;

        case MessageType::MSG_READ_INDEX:
            RAFTPP_LOG_INFO("no leader at term={}; dropping read index msg", m_reader.getTerm());
            break;

        default:
            break;
    }

    return {};
}

Result<void> Raft::StepFollower(Message& m) {
    auto m_reader = capnp_util::reader<msg::Message>(m);
    switch (m_reader.getMsgType()) {
        case MessageType::MSG_PROPOSE:
            if (leader_id_ == kInvalidId) {
                return RaftError(RaftErrorCode::ProposalDropped);
            }
            if (disable_proposal_forwarding_) {
                return RaftError(RaftErrorCode::ProposalDropped);
            }
            capnp_util::builder<msg::Message>(m).setTo(leader_id_);
            Send(m, messages_);
            break;

        case MessageType::MSG_APPEND:
            election_elapsed_ = 0;
            leader_id_ = m_reader.getFrom();
            HandleAppendEntries(m);
            break;

        case MessageType::MSG_HEARTBEAT:
            election_elapsed_ = 0;
            leader_id_ = m_reader.getFrom();
            HandleHeartbeat(m);
            break;

        case MessageType::MSG_SNAPSHOT:
            election_elapsed_ = 0;
            leader_id_ = m_reader.getFrom();
            HandleSnapshot(m);
            break;

        case MessageType::MSG_TRANSFER_LEADER:
            if (leader_id_ == kInvalidId) {
                RAFTPP_LOG_INFO("no leader at term {}; dropping leader transfer msg", term_);
                return {};
            }
            capnp_util::builder<msg::Message>(m).setTo(leader_id_);
            Send(m, messages_);
            break;

        case MessageType::MSG_TIMEOUT_NOW:
            if (promotable_) {
                Hup(true);
            } else {
                RAFTPP_LOG_INFO(
                    "received MsgTimeoutNow from {} but is not promotable", m_reader.getFrom()
                );
            }
            break;

        case MessageType::MSG_READ_INDEX:
            if (leader_id_ == kInvalidId) {
                RAFTPP_LOG_INFO("no leader at term {}; dropping read index msg", term_);
                return {};
            }
            capnp_util::builder<msg::Message>(m).setTo(leader_id_);
            Send(m, messages_);
            break;

        case MessageType::MSG_READ_INDEX_RESP: {
            // Only accept read index responses from the current leader.
            if (leader_id_ == kInvalidId || m_reader.getFrom() != leader_id_) {
                RAFTPP_LOG_DEBUG(
                    "ignored MsgReadIndexResp from {}: leader_id={}", m_reader.getFrom(), leader_id_
                );
                return {};
            }

            auto entries = m_reader.getEntries();
            if (entries.size() != 1) {
                RAFTPP_LOG_ERROR(
                    "invalid format of MsgReadIndexResp from {}, entries_size={}",
                    m_reader.getFrom(), entries.size()
                );
                return {};
            }

            ReadState rs;
            rs.index = m_reader.getIndex();
            auto data = entries[0].getData();
            rs.request_ctx = std::string(reinterpret_cast<const char*>(data.begin()), data.size());

            read_states_.emplace_back(rs);
            std::ignore = raft_log_.MaybeCommit(m_reader.getIndex(), m_reader.getTerm());
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
    auto m_reader = capnp_util::reader<msg::Message>(m);
    Progress* p;
    if (p = progress_tracker_.get(m_reader.getFrom()); p == nullptr) {
        RAFTPP_LOG_INFO("no progress available for {}", m_reader.getFrom());
        return;
    }
    Progress& pr = *p;

    // update followers committed index via heartbeat response
    pr.UpdateCommitted(m_reader.getCommit());
    pr.recent_active() = true;
    RAFTPP_LOG_DEBUG(
        "HandleHeartbeatResponse: from={}, index={}, reject={}", m_reader.getFrom(),
        m_reader.getIndex(), m_reader.getReject()
    );
    pr.Resume();

    if (pr.state() == ProgressState::Replicate && pr.inflights().Full()) {
        pr.inflights().FreeFirstOne();
    }

    // Does it request snapshot?
    RAFTPP_LOG_DEBUG(
        "HandleHeartbeatResp check: from={}, matched={}, next={}, last_index={}",
        m_reader.getFrom(), pr.matched(), pr.next_idx(), raft_log_.LastIndex()
    );
    if (pr.matched() < raft_log_.LastIndex() || pr.pending_request_snapshot() != kInvalidIndex) {
        RAFTPP_LOG_DEBUG("HandleHeartbeatResp: sending append to {}", m_reader.getFrom());
        RaftCore::SendAppend(m_reader.getFrom(), pr, messages_);
    }

    auto context = m_reader.getContext();
    if (read_only_.option() != ReadOnlyOption::Safe || context.size() == 0) {
        return;
    }

    std::string ctx_str(reinterpret_cast<const char*>(context.begin()), context.size());
    const auto acks = read_only_.RecvACK(m_reader.getFrom(), ctx_str);
    if (!acks) {
        return;
    }
    if (!progress_tracker_.HasQuorum(*acks)) {
        return;
    }

    for (const auto& rs : read_only_.Advance(ctx_str)) {
        if (auto r = HandleReadyReadIndex(rs.req, rs.index)) {
            Send(*r, messages_);
        }
    }
}

void Raft::HandleSnapshotStatus(const Message& m) {
    auto m_reader = capnp_util::reader<msg::Message>(m);
    Progress* p = progress_tracker_.get(m_reader.getFrom());
    if (p == nullptr) {
        return;
    }
    Progress& pr = *p;

    if (pr.state() != ProgressState::Snapshot) {
        return;
    }

    if (m_reader.getReject()) {
        RAFTPP_LOG_DEBUG(
            "HandleSnapshotStatus: reject from {}, index={}, reject_hint={}, log_term={}",
            m_reader.getFrom(), m_reader.getIndex(), m_reader.getRejectHint(), m_reader.getLogTerm()
        );
        pr.SnapshotFailure();
        pr.BecomeProbe();
    } else {
        pr.BecomeProbe();
    }

    pr.Pause();
    pr.pending_request_snapshot() = kInvalidIndex;
}

void Raft::HandleUnreachable(const Message& m) {
    auto m_reader = capnp_util::reader<msg::Message>(m);
    Progress* p = progress_tracker_.get(m_reader.getFrom());
    if (p == nullptr) {
        return;
    }
    Progress& pr = *p;

    if (pr.state() == ProgressState::Replicate) {
        pr.BecomeProbe();
    }

    RAFTPP_LOG_INFO("failed to send message to {} because it is unreachable", m_reader.getFrom());
}

void Raft::HandleTransferLeader(const Message& m) {
    auto m_reader = capnp_util::reader<msg::Message>(m);
    const uint64_t from = m_reader.getFrom();
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
    auto m = capnp_util::make<msg::Message>();
    auto m_builder = capnp_util::builder<msg::Message>(m);
    m_builder.setTo(to);
    m_builder.setMsgType(MessageType::MSG_HEARTBEAT);
    m_builder.setCommit(std::min(pr.matched(), raft_log_.committed()));
    if (ctx) {
        m_builder.setContext(
            kj::arrayPtr(reinterpret_cast<const kj::byte*>(ctx->data()), ctx->size())
        );
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
    auto m_reader = capnp_util::reader<msg::Message>(m);
    switch (m_reader.getMsgType()) {
        case MessageType::MSG_BEAT:
            BroadcastHeartbeat();
            return {};

        case MessageType::MSG_CHECK_QUORUM:
            if (!CheckQuorumActive()) {
                RAFTPP_LOG_WARN("stepped down to follower since quorum is not active");
                BecomeFollower(term_, kInvalidId);
            }
            return {};

        case MessageType::MSG_PROPOSE: {
            auto entries = m_reader.getEntries();
            if (entries.size() == 0) {
                PANIC("stepped empty MsgProp");
            }

            if (!progress_tracker_.progress_map().contains(id_)) {
                return RaftError(RaftErrorCode::ProposalDropped);
            }

            if (lead_transferee_) {
                return RaftError(RaftErrorCode::ProposalDropped);
            }

            for (size_t i = 0; i < entries.size(); i++) {
                auto ent = entries[i];
                if (ent.getEntryType() == EntryType::ENTRY_CONF_CHANGE_V2) {
                    // Parse ConfChangeV2 from entry data
                    auto data = ent.getData();
                    kj::ArrayPtr<const kj::byte> data_ptr(data.begin(), data.size());

                    // Validate it can be parsed (just check if data exists for now)
                    if (data.size() == 0) {
                        return RaftError(RaftErrorCode::ProposalDropped);
                    }
                }
            }

            {
                std::vector<Entry> entries_vec;
                entries_vec.reserve(entries.size());
                for (const auto& e : entries) {
                    entries_vec.push_back(capnp_util::clone<msg::Entry>(e));
                }
                if (!AppendEntry(std::move(entries_vec))) {
                    return RaftError(RaftErrorCode::ProposalDropped);
                }
            }
            BroadcastAppend();
            return {};
        }

        case MessageType::MSG_READ_INDEX: {
            if (!CommitToCurrentTerm()) {
                // Reject read only request when this leader has not committed any log entry
                // in its term.
                RAFTPP_LOG_INFO("leader has not yet committed in its term; dropping read index msg"
                );
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
                    auto entries = m_reader.getEntries();
                    if (entries.size() > 0) {
                        auto data = entries[0].getData();
                        std::string ctx(reinterpret_cast<const char*>(data.begin()), data.size());
                        read_only_.AddRequest(raft_log_.committed(), m, id_);
                        BroadcastHeartbeat(ctx);
                    }
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

    // Continue handling other message types
    switch (m_reader.getMsgType()) {
        case MessageType::MSG_APPEND_RESPONSE:
            RAFTPP_LOG_DEBUG("StepLeader: received MsgAppendResponse from {}", m_reader.getFrom());
            HandleAppendResponse(m);
            break;
        case MessageType::MSG_HEARTBEAT_RESPONSE:
            HandleHeartbeatResponse(m);
            break;
        case MessageType::MSG_SNAP_STATUS:
            HandleSnapshotStatus(m);
            break;
        case MessageType::MSG_UNREACHABLE:
            HandleUnreachable(m);
            break;
        case MessageType::MSG_TRANSFER_LEADER:
            HandleTransferLeader(m);
            break;
        default:
            if (progress_tracker_.get(m_reader.getFrom()) == nullptr) {
                RAFTPP_LOG_DEBUG("no progress available for {}", m_reader.getFrom());
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
    auto m_reader = capnp_util::reader<msg::Message>(m);
    if (m_reader.getTerm() == 0) {
        // local message - fall through to process based on current state
    } else if (m_reader.getTerm() > term_) {
        if (m_reader.getMsgType() == MessageType::MSG_REQUEST_VOTE ||
            m_reader.getMsgType() == MessageType::MSG_REQUEST_PRE_VOTE) {
            auto ctx = m_reader.getContext();
            std::string ctx_str(reinterpret_cast<const char*>(ctx.begin()), ctx.size());
            const bool force = (ctx_str == kCampaignTransfer);
            const bool in_lease =
                check_quorum_ && leader_id_ != kInvalidId && election_elapsed_ < election_timeout_;

            if (!force && in_lease) {
                RAFTPP_LOG_INFO("ignored vote from {}: lease is not expired");
                return {};
            }
        }

        if (m_reader.getMsgType() == MessageType::MSG_REQUEST_PRE_VOTE ||
            (m_reader.getMsgType() == MessageType::MSG_REQUEST_PRE_VOTE_RESPONSE &&
             !m_reader.getReject())) {
            // For a pre-vote request:
            // Never change our term in response to a pre-vote request.
        } else {
            RAFTPP_LOG_INFO("received a message with higher term from {}", m_reader.getFrom());
            if (m_reader.getMsgType() == MessageType::MSG_APPEND ||
                m_reader.getMsgType() == MessageType::MSG_HEARTBEAT ||
                m_reader.getMsgType() == MessageType::MSG_SNAPSHOT) {
                BecomeFollower(m_reader.getTerm(), m_reader.getFrom());
            } else {
                BecomeFollower(m_reader.getTerm(), kInvalidId);
            }
        }
        // Fall through to process the message
    } else if (m_reader.getTerm() < term_) {
        if ((check_quorum_ || pre_vote_) &&
            (m_reader.getMsgType() == MessageType::MSG_HEARTBEAT ||
             m_reader.getMsgType() == MessageType::MSG_APPEND)) {
            auto to_send = capnp_util::make<msg::Message>();
            auto to_send_builder = capnp_util::builder<msg::Message>(to_send);
            to_send_builder.setTo(m_reader.getFrom());
            to_send_builder.setMsgType(MessageType::MSG_APPEND_RESPONSE);
            Send(to_send, messages_);
        } else if (m_reader.getMsgType() == MessageType::MSG_REQUEST_PRE_VOTE) {
            auto to_send = capnp_util::make<msg::Message>();
            auto to_send_builder = capnp_util::builder<msg::Message>(to_send);
            to_send_builder.setTo(m_reader.getFrom());
            to_send_builder.setMsgType(MessageType::MSG_REQUEST_PRE_VOTE_RESPONSE);
            to_send_builder.setReject(true);
            to_send_builder.setTerm(term_);
            Send(to_send, messages_);
        } else {
            // ignore other cases
            RAFTPP_LOG_INFO("ignored a message with lower term, from={}", m_reader.getFrom());
        }
        return {};
    }

    // m.term() == term_
    switch (m_reader.getMsgType()) {
        case MessageType::MSG_HUP:
            Hup(false);
            return {};

        case MessageType::MSG_REQUEST_VOTE:
        case MessageType::MSG_REQUEST_PRE_VOTE: {
            const bool can_vote = (vote_ == m_reader.getFrom()) ||
                (vote_ == kInvalidId && leader_id_ == kInvalidId) ||
                (m_reader.getMsgType() == MessageType::MSG_REQUEST_PRE_VOTE &&
                 m_reader.getTerm() > term_);

            if (can_vote && raft_log_.IsUpToDate(m_reader.getIndex(), m_reader.getLogTerm()) &&
                (m_reader.getIndex() > raft_log_.LastIndex() || priority_ <= m_reader.getPriority()
                )) {
                auto to_send = capnp_util::make<msg::Message>();
                auto to_send_builder = capnp_util::builder<msg::Message>(to_send);
                to_send_builder.setTo(m_reader.getFrom());
                to_send_builder.setMsgType(VoteRespMsgType(m_reader.getMsgType()));
                to_send_builder.setReject(false);
                to_send_builder.setTerm(m_reader.getTerm());
                Send(to_send, messages_);

                if (m_reader.getMsgType() == MessageType::MSG_REQUEST_VOTE) {
                    // Only record real votes.
                    election_elapsed_ = 0;
                    vote_ = m_reader.getFrom();
                }
            } else {
                auto to_send = capnp_util::make<msg::Message>();
                auto to_send_builder = capnp_util::builder<msg::Message>(to_send);
                to_send_builder.setTo(m_reader.getFrom());
                to_send_builder.setMsgType(VoteRespMsgType(m_reader.getMsgType()));
                to_send_builder.setReject(true);
                to_send_builder.setTerm(term_);

                const auto [commit, commit_term] = raft_log_.CommitInfo();
                to_send_builder.setCommit(commit);
                to_send_builder.setCommitTerm(commit_term);
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
    auto m_reader = capnp_util::reader<msg::Message>(m);
    if (pending_request_snapshot_ != kInvalidIndex) {
        SendRequestSnapshot();
        return;
    }

    if (m_reader.getIndex() < raft_log_.committed()) {
        auto to_send = capnp_util::make<msg::Message>();
        auto to_send_builder = capnp_util::builder<msg::Message>(to_send);
        to_send_builder.setTo(m_reader.getFrom());
        to_send_builder.setMsgType(MessageType::MSG_APPEND_RESPONSE);
        to_send_builder.setIndex(raft_log_.committed());
        to_send_builder.setCommit(raft_log_.committed());
        Send(to_send, messages_);
        return;
    }

    auto to_send = capnp_util::make<msg::Message>();
    auto to_send_builder = capnp_util::builder<msg::Message>(to_send);
    to_send_builder.setTo(m_reader.getFrom());
    to_send_builder.setMsgType(MessageType::MSG_APPEND_RESPONSE);

    RAFTPP_LOG_INFO(
        "HandleAppendEntries: index={}, log_term={}, commit={}, num_entries={}",
        m_reader.getIndex(), m_reader.getLogTerm(), m_reader.getCommit(),
        m_reader.getEntries().size()
    );

    // Convert entries to vector
    auto entries_list = m_reader.getEntries();
    std::vector<Entry> entries_vec;
    entries_vec.reserve(entries_list.size());
    for (const auto& e : entries_list) {
        entries_vec.push_back(capnp_util::clone<msg::Entry>(e));
    }

    const auto r = raft_log_.MaybeAppend(
        m_reader.getIndex(), m_reader.getLogTerm(), m_reader.getCommit(), entries_vec
    );
    if (!r) {
        // Fatal error (e.g., conflict with committed entry)
        PANIC("MaybeAppend returned error: {}", r.error());
    } else if (r->term_matched) {
        to_send_builder.setIndex(r->last_index);
    } else {
        const auto [hint_index, hint_term] = raft_log_.FindConflictByTerm(
            std::min(m_reader.getIndex(), raft_log_.LastIndex()), m_reader.getLogTerm()
        );

        if (!hint_term.has_value()) {
            PANIC("term({}) must be valid", hint_index);
        }

        to_send_builder.setIndex(m_reader.getIndex());
        to_send_builder.setReject(true);
        to_send_builder.setRejectHint(hint_index);
        to_send_builder.setLogTerm(*hint_term);
    }

    to_send_builder.setCommit(raft_log_.committed());
    Send(to_send, messages_);
}

bool Raft::TickElection() {
    heartbeat_elapsed_ += 1;
    election_elapsed_ += 1;

    bool has_ready = false;
    if (election_elapsed_ >= randomized_election_timeout_) {
        election_elapsed_ = 0;
        auto m = capnp_util::make<msg::Message>();
        auto m_builder = capnp_util::builder<msg::Message>(m);
        m_builder.setTo(kInvalidId);
        m_builder.setMsgType(MessageType::MSG_HUP);
        m_builder.setFrom(id_);
        has_ready = true;
        std::ignore = Step(m);
    }

    if (state_ != StateRole::Leader) {
        return has_ready;
    }

    if (heartbeat_elapsed_ >= heartbeat_timeout_) {
        heartbeat_elapsed_ = 0;
        has_ready = true;
        auto m = capnp_util::make<msg::Message>();
        auto m_builder = capnp_util::builder<msg::Message>(m);
        m_builder.setTo(kInvalidId);
        m_builder.setMsgType(MessageType::MSG_BEAT);
        m_builder.setFrom(id_);
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
            auto m = capnp_util::make<msg::Message>();
            auto m_builder = capnp_util::builder<msg::Message>(m);
            m_builder.setTo(kInvalidId);
            m_builder.setMsgType(MessageType::MSG_CHECK_QUORUM);
            m_builder.setFrom(id_);
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
        auto m = capnp_util::make<msg::Message>();
        auto m_builder = capnp_util::builder<msg::Message>(m);
        m_builder.setTo(kInvalidId);
        m_builder.setMsgType(MessageType::MSG_BEAT);
        m_builder.setFrom(id_);
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
        RAFTPP_LOG_WARN(
            "try to reduce uncommitted size less than 0, first index of pending ents is {}",
            capnp_util::reader<msg::Entry>(ents.front()).getIndex()
        );
    }
}

void Raft::CommitApply(const uint64_t applied) {
    CommitApplyInternal(applied, false);
}

Result<void> Raft::RequestSnapshot() {
    if (state_ == StateRole::Leader) {
        RAFTPP_LOG_INFO("can not request snapshot on leader; dropping request snapshot");
    } else if (leader_id_ == kInvalidId) {
        RAFTPP_LOG_INFO("no leader; dropping request snapshot, term={}", term_);
    } else if (snapshot().has_value() || pending_request_snapshot_ != kInvalidIndex) {
        RAFTPP_LOG_INFO("there is a pending snapshot; dropping request snapshot");
    } else {
        const auto request_index = raft_log_.LastIndex();
        const auto request_index_term = Unwrap(raft_log_.Term(request_index));
        if (term_ == request_index_term) {
            pending_request_snapshot_ = request_index;
            SendRequestSnapshot();
            return {};
        }
        RAFTPP_LOG_INFO(
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
    return capnp_util::make<msg::HardState>([this](auto hs_builder) {
        hs_builder.setTerm(term_);
        hs_builder.setVote(vote_);
        hs_builder.setCommit(raft_log_.committed());
    });
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
    auto cc_reader = capnp_util::reader<msg::ConfChangeV2>(cc);
    return cc_reader.getTransition() == ConfChangeTransition::AUTO &&
        cc_reader.getChanges().size() == 0;
}

std::optional<bool> EnterJoint(const ConfChangeV2& cc) {
    auto cc_reader = capnp_util::reader<msg::ConfChangeV2>(cc);
    if (cc_reader.getTransition() != ConfChangeTransition::AUTO ||
        cc_reader.getChanges().size() > 1) {
        switch (cc_reader.getTransition()) {
            case ConfChangeTransition::AUTO:
            case ConfChangeTransition::IMPLICIT:
                return true;
            case ConfChangeTransition::EXPLICIT:
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
        RAFTPP_LOG_INFO("ApplyConfChange: LeaveJoint");
        r = changer.LeaveJoint();
    } else {
        auto cc_reader = capnp_util::reader<msg::ConfChangeV2>(cc);
        auto changes_list = cc_reader.getChanges();
        std::vector<ConfChangeSingle> ccs;
        ccs.reserve(changes_list.size());
        for (const auto& c : changes_list) {
            auto single = capnp_util::make<msg::ConfChangeSingle>();
            auto single_builder = capnp_util::builder<msg::ConfChangeSingle>(single);
            single_builder.setChangeType(c.getChangeType());
            single_builder.setNodeId(c.getNodeId());
            ccs.push_back(std::move(single));
        }
        if (const auto auto_leave = EnterJoint(cc)) {
            RAFTPP_LOG_INFO("ApplyConfChange: EnterJoint, auto_leave={}", *auto_leave);
            r = changer.EnterJoint(*auto_leave, ccs);
        } else {
            RAFTPP_LOG_INFO("ApplyConfChange: Simple, num_changes={}", ccs.size());
            r = changer.Simple(ccs);
        }
    }

    if (r) {
        const auto& cfg = r->first;
        const auto& changes = r->second;
        RAFTPP_LOG_INFO("ApplyConfChange: success, num_changes={}", changes.size());
        for (const auto& [id, change_type] : changes) {
            RAFTPP_LOG_INFO(
                "  change: id={}, type={}", id, change_type == MapChangeType::Add ? "Add" : "Remove"
            );
        }
        progress_tracker_.ApplyConf(cfg, changes, raft_log_.LastIndex());
    } else {
        RAFTPP_LOG_ERROR("ApplyConfChange: failed, error={}", r.error().ToString());
    }

    return PostConfChange();
}

MessageType Raft::VoteRespMsgType(const MessageType mt) {
    switch (mt) {
        case MessageType::MSG_REQUEST_VOTE:
            return MessageType::MSG_REQUEST_VOTE_RESPONSE;
        case MessageType::MSG_REQUEST_PRE_VOTE:
            return MessageType::MSG_REQUEST_PRE_VOTE_RESPONSE;
        default:
            PANIC("not a vote message: {}", static_cast<int>(mt));
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
    RAFTPP_LOG_INFO("reset election timeout, {} -> {}", prev_timeout, timeout);
}

void Raft::AbortLeaderTransfer() {
    lead_transferee_ = {};
}

void Raft::Reset(const uint64_t term) {
    if (term_ != term) {
        term_ = term;
        vote_ = kInvalidId;
    }
    leader_id_ = kInvalidId;
    ResetRandomizedElectionTimeout();
    election_elapsed_ = 0;
    heartbeat_elapsed_ = 0;

    AbortLeaderTransfer();
    progress_tracker_.ResetVotes();

    pending_conf_index_ = 0;
    read_only_ = ReadOnly(read_only_.option());
    pending_request_snapshot_ = kInvalidIndex;

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

    RAFTPP_LOG_INFO("became follower, term={}, from_role={}", term, format_as(from_role));
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

void Raft::MaybeFreeInflightBuffers() {
    for (auto& [id, pr] : progress_tracker_.progress_map()) {
        if (pr.inflights().Count() == 0 && pr.inflights().buffer_is_allocated()) {
            // Free the buffer if this peer has no inflight messages
            pr.inflights().Reset();
        }
    }
}

void Raft::AdjustMaxInflightMsgs(uint64_t id, size_t max_inflight) {
    auto* pr = progress_tracker_.get(id);
    if (pr != nullptr) {
        pr->inflights().SetCapacity(max_inflight);
    }
}

bool Raft::ConfStatesEqualIgnoringOrder(const ConfState& a, const ConfState& b) {
    auto a_reader = capnp_util::reader<msg::ConfState>(a);
    auto b_reader = capnp_util::reader<msg::ConfState>(b);

    // Compare voters
    auto a_voters_list = a_reader.getVoters();
    auto b_voters_list = b_reader.getVoters();
    if (a_voters_list.size() != b_voters_list.size()) {
        return false;
    }
    std::vector<uint64_t> a_voters;
    std::vector<uint64_t> b_voters;
    for (auto v : a_voters_list)
        a_voters.push_back(v);
    for (auto v : b_voters_list)
        b_voters.push_back(v);
    std::sort(a_voters.begin(), a_voters.end());
    std::sort(b_voters.begin(), b_voters.end());
    if (a_voters != b_voters) {
        return false;
    }

    // Compare learners
    auto a_learners_list = a_reader.getLearners();
    auto b_learners_list = b_reader.getLearners();
    if (a_learners_list.size() != b_learners_list.size()) {
        return false;
    }
    std::vector<uint64_t> a_learners;
    std::vector<uint64_t> b_learners;
    for (auto l : a_learners_list)
        a_learners.push_back(l);
    for (auto l : b_learners_list)
        b_learners.push_back(l);
    std::sort(a_learners.begin(), a_learners.end());
    std::sort(b_learners.begin(), b_learners.end());
    if (a_learners != b_learners) {
        return false;
    }

    // Voters outgoing and incoming for joint configs
    auto a_outgoing_list = a_reader.getVotersOutgoing();
    auto b_outgoing_list = b_reader.getVotersOutgoing();
    if (a_outgoing_list.size() != b_outgoing_list.size()) {
        return false;
    }
    std::vector<uint64_t> a_outgoing;
    std::vector<uint64_t> b_outgoing;
    for (auto v : a_outgoing_list)
        a_outgoing.push_back(v);
    for (auto v : b_outgoing_list)
        b_outgoing.push_back(v);
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
