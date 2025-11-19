#include "raftpp/raw_node.h"

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

raftpp::RawNode::RawNode(const Config& config, std::unique_ptr<Storage> store) : raft_(config, std::move(store)) {}
