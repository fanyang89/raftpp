#include "raftpp/raw_node.h"

#include <gtest/gtest.h>

using namespace raftpp;

TEST(RawNowUtils, IsLocalMessage) {
    EXPECT_TRUE(IsLocalMessage(MsgHup));
    EXPECT_TRUE(IsLocalMessage(MsgBeat));
    EXPECT_TRUE(IsLocalMessage(MsgUnreachable));
    EXPECT_TRUE(IsLocalMessage(MsgSnapStatus));
    EXPECT_TRUE(IsLocalMessage(MsgCheckQuorum));

    EXPECT_FALSE(IsLocalMessage(MsgPropose));
    EXPECT_FALSE(IsLocalMessage(MsgAppend));
    EXPECT_FALSE(IsLocalMessage(MsgAppendResponse));
    EXPECT_FALSE(IsLocalMessage(MsgRequestVote));
    EXPECT_FALSE(IsLocalMessage(MsgRequestVoteResponse));
    EXPECT_FALSE(IsLocalMessage(MsgSnapshot));
    EXPECT_FALSE(IsLocalMessage(MsgHeartbeat));
    EXPECT_FALSE(IsLocalMessage(MsgHeartbeatResponse));
    EXPECT_FALSE(IsLocalMessage(MsgTransferLeader));
    EXPECT_FALSE(IsLocalMessage(MsgTimeoutNow));
    EXPECT_FALSE(IsLocalMessage(MsgReadIndex));
    EXPECT_FALSE(IsLocalMessage(MsgReadIndexResp));
    EXPECT_FALSE(IsLocalMessage(MsgRequestPreVote));
    EXPECT_FALSE(IsLocalMessage(MsgRequestPreVoteResponse));
}
