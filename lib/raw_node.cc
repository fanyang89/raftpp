#include "raftpp/raw_node.h"

#include <google/protobuf/util/message_differencer.h>

#include "raftpp/util.h"

namespace raftpp {

bool IsLocalMessage(const MessageType t) {
    switch (t) {
        case MsgHup:
        case MsgBeat:
        case MsgUnreachable:
        case MsgSnapStatus:
        case MsgCheckQuorum:
            return true;
        default:
            return false;
    }
}

bool IsResponseMessage(const MessageType t) {
    switch (t) {
        case MsgAppendResponse:
        case MsgRequestVoteResponse:
        case MsgHeartbeatResponse:
        case MsgUnreachable:
        case MsgRequestPreVoteResponse:
            return true;
        default:
            return false;
    }
}

RawNode::RawNode(const Config& config, std::unique_ptr<Storage> store)
    : raft_(config, std::move(store)), max_number_(0), commit_since_index_(config.applied) {
    ASSERT(config.id, "config.id must not be zero");
    prev_hs_ = raft_.hard_state();
    prev_ss_ = raft_.soft_state();
}

void RawNode::SetPriority(uint64_t priority) {
    raft_.SetPriority(priority);
}

Result<void> RawNode::RequestSnapshot() {
    return raft_.RequestSnapshot();
}

void RawNode::TransferLeader(const uint64_t transferee) {
    Message m;
    m.set_msg_type(MsgTransferLeader);
    m.set_from(transferee);
    std::ignore = raft_.Step(m);
}

void RawNode::ReadIndex(const std::string& ctx) {
    Message m;
    m.set_msg_type(MsgReadIndex);
    auto* e = m.mutable_entries()->Add();
    e->set_data(ctx);
    std::ignore = raft_.Step(m);
}

Ready RawNode::GetReady() {
    ++max_number_;

    Ready rd;
    rd.number = max_number_;

    ReadyRecord rd_record;
    rd_record.number = max_number_;

    if (prev_ss_.raft_state != StateRole::Leader && raft_.state() == StateRole::Leader) {
        const auto records = records_;
        records_.clear();
        for (auto& r : records) {
            ASSERT(r.number, std::nullopt);
            ASSERT(r.snapshot, std::nullopt);
        }
    }

    const auto ss = raft_.soft_state();
    if (ss != prev_ss_) {
        rd.ss = ss;
    }

    const auto hs = raft_.hard_state();
    if (hs != prev_hs_) {
        if (hs.vote() != prev_hs_.vote() || hs.term() != prev_hs_.term()) {
            rd.must_sync = true;
        }
        rd.hs = hs;
    }

    if (!raft_.read_states().empty()) {
        rd.read_states = raft_.read_states();
        raft_.read_states().clear();
    }

    if (const auto snapshot = raft_.raft_log().unstable().snapshot()) {
        rd.snapshot = *snapshot;
        ASSERT(commit_since_index_ <= rd.snapshot.metadata().index());
        commit_since_index_ = rd.snapshot.metadata().index();
        ASSERT(
            !raft_.raft_log().HasNextEntriesSince(commit_since_index_),
            "has snapshot but also has committed entries since {}", commit_since_index_
        );
        rd_record.snapshot = {rd.snapshot.metadata().index(), rd.snapshot.metadata().term()};
        rd.must_sync = true;
    }

    rd.entries = raft_.raft_log().unstable().entries();
    if (!rd.entries.empty()) {
        rd.must_sync = true;
        const auto& last = rd.entries.back();
        rd_record.last_entry = {last.index(), last.term()};
    }

    rd.is_persisted_msg = raft_.state() != StateRole::Leader;
    rd.light = GetLightReady();
    records_.emplace_back(rd_record);
    return rd;
}

bool RawNode::HasReady() const {
    if (!raft_.messages().empty()) {
        return true;
    }

    if (raft_.soft_state() != prev_ss_) {
        return true;
    }

    if (raft_.hard_state() != prev_hs_) {
        return true;
    }

    if (!raft_.read_states().empty()) {
        return true;
    }

    if (!raft_.raft_log().unstable().entries().empty()) {
        return true;
    }

    if (const auto snapshot = raft_.snapshot()) {
        if (snapshot->has_metadata()) {
            return true;
        }
    }

    if (raft_.raft_log().HasNextEntriesSince(commit_since_index_)) {
        return true;
    }

    return false;
}

void RawNode::OnPersistReady(uint64_t number) {
    uint64_t index = 0;
    uint64_t term = 0;
    uint64_t snap_index = 0;

    while (!records_.empty()) {
        const auto record = records_.front();
        records_.pop_front();

        if (record.number > number) {
            break;
        }

        if (const auto snapshot = record.snapshot) {
            snap_index = snapshot->first;
            index = 0;
            term = 0;
        }

        if (const auto last_entry = record.last_entry) {
            index = last_entry->first;
            term = last_entry->second;
        }
    }

    if (snap_index != 0) {
        raft_.OnPersistSnapshot(snap_index);
    }

    if (index != 0) {
        raft_.OnPersistEntries(index, term);
    }
}

LightReady RawNode::AdvanceAppend(const Ready& rd) {
    CommitReady(rd);
    OnPersistReady(max_number_);
    LightReady light_rd = GetLightReady();

    if (raft_.state() != StateRole::Leader && !light_rd.messages.empty()) {
        PANIC("not leader but has new msg after advance");
    }

    const auto hard_state = raft_.hard_state();
    if (hard_state.commit() > prev_hs_.commit()) {
        light_rd.commit_index = hard_state.commit();
        prev_hs_.set_commit(hard_state.commit());
    } else {
        ASSERT(hard_state.commit() == prev_hs_.commit());
        light_rd.commit_index = {};
    }

    ASSERT(hard_state == prev_hs_, "hard state != prev_hs");
    return light_rd;
}

void RawNode::AdvanceApplyTo(const uint64_t applied) {
    raft_.CommitApply(applied);
}

void RawNode::AdvanceAppendAsync(const Ready& rd) {
    CommitReady(rd);
}

void RawNode::CommitReady(const Ready& rd) {
    if (const auto ss = rd.ss) {
        prev_ss_ = *ss;
    }

    if (const auto hs = rd.hs) {
        prev_hs_ = *hs;
    }

    const auto rd_record = records_.back();
    ASSERT(rd_record.number == rd.number);

    if (const auto snapshot = rd_record.snapshot) {
        const auto index = snapshot->first;
        raft_.raft_log().StableSnapshot(index);
    }

    if (const auto last_entry = rd_record.last_entry) {
        const auto index = last_entry->first;
        const auto term = last_entry->second;
        raft_.raft_log().StableEntries(index, term);
    }
}

void RawNode::AdvanceApply() {
    raft_.CommitApply(commit_since_index_);
}

LightReady RawNode::Advance(const Ready& rd) {
    const auto applied = commit_since_index_;
    const auto light_rd = AdvanceAppend(rd);
    AdvanceApplyTo(applied);
    return light_rd;
}

LightReady RawNode::GetLightReady() {
    LightReady rd;
    const auto max_size = raft_.max_committed_size_per_ready();

    if (const auto committed_entries = raft_.raft_log().NextEntriesSince(commit_since_index_, max_size)) {
        rd.committed_entries = *committed_entries;
    }

    raft_.ReduceUncommittedSize(rd.committed_entries);

    if (!rd.committed_entries.empty()) {
        const auto e = rd.committed_entries.back();
        ASSERT(commit_since_index_ < e.index());
        commit_since_index_ = e.index();
    }

    if (!raft_.messages().empty()) {
        rd.messages = raft_.messages();
        raft_.messages().clear();
    }

    return rd;
}

bool RawNode::Tick() {
    return raft_.Tick();
}

Result<void> RawNode::Campaign() {
    Message m;
    m.set_msg_type(MsgHup);
    return raft_.Step(m);
}

Result<void> RawNode::Propose(const std::string& ctx, const std::string& data) {
    Message m;
    m.set_msg_type(MsgPropose);
    m.set_from(raft_.id());
    Entry e;
    e.set_data(data);
    e.set_context(ctx);
    m.mutable_entries()->Add()->CopyFrom(e);
    return raft_.Step(m);
}

void RawNode::Ping() {
    return raft_.Ping();
}

Result<void> RawNode::ProposeConfChange(const std::string& ctx, const ConfChangeV2& cc) {
    Message m;
    m.set_msg_type(MsgPropose);
    Entry e;
    e.set_entry_type(EntryConfChangeV2);
    e.set_data(cc.SerializeAsString());
    e.set_context(ctx);
    m.mutable_entries()->Add()->CopyFrom(e);
    return raft_.Step(m);
}

Result<ConfState> RawNode::ApplyConfChange(const ConfChangeV2& cc) {
    return raft_.ApplyConfChange(cc);
}

Result<void> RawNode::Step(Message m) {
    if (IsLocalMessage(m.msg_type())) {
        return RaftError(RaftErrorCode::StepLocalMsg);
    }

    if (raft_.progress_tracker().get(m.from()) != nullptr || !IsResponseMessage(m.msg_type())) {
        return raft_.Step(m);
    }

    return RaftError(RaftErrorCode::StepPeerNotFound);
}

void RawNode::OnEntriesFetched(const GetEntriesContext& ctx) {
    switch (ctx.what) {
        case GetEntriesFor::SendAppend: {
            const auto to = ctx.payload.send_append.to;
            const auto term = ctx.payload.send_append.term;
            const auto aggressively = ctx.payload.send_append.aggressively;
            if (raft_.term() != term || raft_.state() != StateRole::Leader) {
                return;
            }

            if (raft_.progress_tracker().get(to) == nullptr) {
                return;
            }

            if (aggressively) {
                raft_.SendAppendAggressively(to);
            } else {
                raft_.SendAppend(to);
            }

            break;
        }

        case GetEntriesFor::Empty:
            break;

        default:
            PANIC("shouldn't call callback on non-async context");
    }
}

}  // namespace raftpp
