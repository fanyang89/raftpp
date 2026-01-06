#include "raftpp/raw_node.h"

#include <doctest/doctest.h>

#include "raftpp/memory_storage.h"
#include "raftpp/raft_config.h"
#include "test_util.h"

using namespace raftpp;

TEST_SUITE_BEGIN("raw_node");

TEST_CASE("raw_node: is local message") {
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

TEST_CASE("raw_node: step") {
    const std::vector<MessageType> msg_types{
        MsgHup,
        MsgBeat,
        MsgPropose,
        MsgAppend,
        MsgAppendResponse,
        MsgRequestVote,
        MsgRequestVoteResponse,
        MsgSnapshot,
        MsgHeartbeat,
        MsgHeartbeatResponse,
        MsgUnreachable,
        MsgSnapStatus,
        MsgCheckQuorum,
        MsgTransferLeader,
        MsgTimeoutNow,
        MsgReadIndex,
        MsgReadIndexResp,
        MsgRequestPreVote,
        MsgRequestPreVoteResponse
    };

    for (const auto msg_t : msg_types) {
        auto storage = std::make_unique<MemoryStorage>();
        HardState hs;
        hs.set_term(1);
        hs.set_vote(0);
        hs.set_commit(1);
        storage->SetRaftState({hs, {}});

        const auto append_res = storage->Append({NewEntry(1, 1)});
        REQUIRE(append_res);

        Snapshot snap;
        snap.mutable_metadata()->set_index(1);
        snap.mutable_metadata()->set_term(1);
        snap.mutable_metadata()->mutable_conf_state()->mutable_voters()->Add(1);
        const auto snap_res = storage->ApplySnapshot(snap);
        REQUIRE(snap_res);

        Config config = DefaultConfig();
        config.id = 1;
        config.election_tick = 10;
        config.heartbeat_tick = 1;

        RawNode raw_node(config, std::move(storage));

        Message m;
        m.set_to(0);
        m.set_from(0);
        m.set_msg_type(msg_t);
        m.set_term(0);

        const auto res = raw_node.Step(m);

        if (IsLocalMessage(msg_t)) {
            CHECK_FALSE(res);
            CHECK(res.error().Is(RaftErrorCode::StepLocalMsg));
        }
    }
}

TEST_SUITE_END();
