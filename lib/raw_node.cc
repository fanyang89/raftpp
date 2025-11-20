#include "raftpp/raw_node.h"

namespace raftpp {

bool raftpp::IsLocalMessage(MessageType t) {
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

bool raftpp::IsResponseMessage(MessageType t) {
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

}  // namespace raftpp
