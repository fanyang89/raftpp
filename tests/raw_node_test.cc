#include "raftpp/raw_node.h"

#include <doctest/doctest.h>

using namespace raftpp;

TEST_SUITE_BEGIN("RawNode");

TEST_CASE("Is local message") {
    CHECK(IsLocalMessage(MsgHup));
    CHECK(IsLocalMessage(MsgBeat));
    CHECK(IsLocalMessage(MsgUnreachable));
    CHECK(IsLocalMessage(MsgSnapStatus));
    CHECK(IsLocalMessage(MsgCheckQuorum));

    CHECK_FALSE(IsLocalMessage(MsgPropose));
    CHECK_FALSE(IsLocalMessage(MsgAppend));
    CHECK_FALSE(IsLocalMessage(MsgAppendResponse));
    CHECK_FALSE(IsLocalMessage(MsgRequestVote));
    CHECK_FALSE(IsLocalMessage(MsgRequestVoteResponse));
    CHECK_FALSE(IsLocalMessage(MsgSnapshot));
    CHECK_FALSE(IsLocalMessage(MsgHeartbeat));
    CHECK_FALSE(IsLocalMessage(MsgHeartbeatResponse));
    CHECK_FALSE(IsLocalMessage(MsgTransferLeader));
    CHECK_FALSE(IsLocalMessage(MsgTimeoutNow));
    CHECK_FALSE(IsLocalMessage(MsgReadIndex));
    CHECK_FALSE(IsLocalMessage(MsgReadIndexResp));
    CHECK_FALSE(IsLocalMessage(MsgRequestPreVote));
    CHECK_FALSE(IsLocalMessage(MsgRequestPreVoteResponse));
}

TEST_SUITE_END();
