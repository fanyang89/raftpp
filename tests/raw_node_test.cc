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

TEST_CASE("raw_node: step local message ignored") {
    const std::vector<MessageType> local_msg_types{
        MsgHup, MsgBeat, MsgUnreachable, MsgSnapStatus, MsgCheckQuorum,
    };

    for (const auto msg_t : local_msg_types) {
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
        m.set_term(1);

        const auto res = raw_node.Step(m);
        CHECK_FALSE(res);
        CHECK(res.error().Is(RaftErrorCode::StepLocalMsg));
    }
}

TEST_CASE("raw_node: propose data") {
    auto storage = std::make_unique<MemoryStorage>();

    Config config = DefaultConfig();
    config.id = 1;
    config.election_tick = 10;
    config.heartbeat_tick = 1;

    ConfState conf_state;
    conf_state.add_voters(1);
    HardState hard_state;
    hard_state.set_commit(0);
    hard_state.set_term(0);
    hard_state.set_vote(0);
    storage->SetRaftState({hard_state, conf_state});

    RawNode raw_node(config, std::move(storage));

    raw_node.Campaign().value();

    while (true) {
        auto rd = raw_node.GetReady();
        if (rd.ss.has_value() && rd.ss->leader_id == 1) {
            raw_node.Advance(rd);
            break;
        }
        raw_node.Advance(rd);
    }

    auto result = raw_node.Propose("", "testdata");
    CHECK(result);

    auto rd = raw_node.GetReady();
    CHECK(rd.entries.size() >= 1);
    CHECK_EQ(rd.entries.back().data(), "testdata");
    raw_node.Advance(rd);
}

TEST_CASE("raw_node: set priority") {
    auto storage = std::make_unique<MemoryStorage>();

    Config config = DefaultConfig();
    config.id = 1;
    config.election_tick = 10;
    config.heartbeat_tick = 1;

    ConfState conf_state;
    conf_state.add_voters(1);
    HardState hard_state;
    hard_state.set_commit(0);
    storage->SetRaftState({hard_state, conf_state});

    RawNode raw_node(config, std::move(storage));

    std::vector<uint64_t> priorities = {0, 1, 5, 10, 10000};
    for (const auto p : priorities) {
        raw_node.SetPriority(p);
    }
}

// Helper function to prepare async entries test
void PrepareAsyncEntries(RawNode& raw_node, const std::shared_ptr<MemoryStorage>& storage) {
    // Become leader directly (like raft-rs: raw_node.raft.become_candidate(); raw_node.raft.become_leader();)
    raw_node.raft().BecomeCandidate();
    raw_node.raft().BecomeLeader();

    auto rd = raw_node.GetReady();
    std::ignore = storage->Append(rd.entries);
    raw_node.Advance(rd);

    // Propose 10 entries
    std::string data(1000, '\x01');
    for (int i = 0; i < 10; i++) {
        raw_node.Propose("", data).value();
    }

    rd = raw_node.GetReady();
    CHECK_EQ(rd.entries.size(), 10);
    std::ignore = storage->Append(rd.entries);
    auto msgs = rd.Messages();
    // First append has two entries: the empty entry to confirm the election,
    // and the first proposal (only one proposal gets sent because we're in probe state).
    CHECK_EQ(msgs.size(), 1);
    CHECK_EQ(msgs[0].msg_type(), MsgAppend);
    CHECK_EQ(msgs[0].entries_size(), 2);
    raw_node.AdvanceAppend(rd);

    // Enable "slow storage" - next fetch will be async
    storage->TriggerLogUnavailable(true);

    // Become replicate state by sending append response
    // The term should match the leader's term (1 after BecomeCandidate+BecomeLeader)
    Message append_response;
    append_response.set_from(2);
    append_response.set_to(1);
    append_response.set_msg_type(MsgAppendResponse);
    append_response.set_term(1);
    append_response.set_index(2);
    raw_node.Step(append_response).value();
}

// Test entries are handled properly when they are fetched asynchronously
TEST_CASE("raw_node: async entry fetching") {
    auto storage = std::make_shared<MemoryStorage>();

    Config config = DefaultConfig();
    config.id = 1;
    config.election_tick = 10;
    config.heartbeat_tick = 1;
    config.max_size_per_message = 2048;

    ConfState conf_state;
    conf_state.add_voters(1);
    conf_state.add_voters(2);
    HardState hard_state;
    hard_state.set_commit(0);
    storage->SetRaftState({hard_state, conf_state});

    RawNode raw_node(config, storage);

    PrepareAsyncEntries(raw_node, storage);

    // No entries are sent because the entries are temporarily unavailable
    auto rd = raw_node.GetReady();
    std::ignore = storage->Append(rd.entries);
    auto msgs = rd.Messages();
    CHECK_EQ(msgs.size(), 0);
    raw_node.AdvanceAppend(rd);

    // Entries are sent when the entries are ready which is informed by `on_entries_fetched`.
    storage->TriggerLogUnavailable(false);
    auto context = storage->TakeGetEntriesContext();
    CHECK(context.has_value());
    raw_node.OnEntriesFetched(context.value());

    rd = raw_node.GetReady();
    std::ignore = storage->Append(rd.entries);
    msgs = rd.Messages();
    CHECK(msgs.size() > 0);
    CHECK_EQ(msgs[0].msg_type(), MsgAppend);
    CHECK(msgs[0].entries_size() > 0);
    raw_node.AdvanceAppend(rd);
}

// Test if async fetch entries works well when there is a remove node conf-change.
TEST_CASE("raw_node: async entry fetching to removed node") {
    auto storage = std::make_shared<MemoryStorage>();

    Config config = DefaultConfig();
    config.id = 1;
    config.election_tick = 10;
    config.heartbeat_tick = 1;
    config.max_size_per_message = 2048;

    ConfState conf_state;
    conf_state.add_voters(1);
    conf_state.add_voters(2);
    HardState hard_state;
    hard_state.set_commit(0);
    storage->SetRaftState({hard_state, conf_state});

    RawNode raw_node(config, storage);

    PrepareAsyncEntries(raw_node, storage);

    // Remove node 2
    ConfChangeV2 cc;
    auto* change = cc.add_changes();
    change->set_change_type(RemoveNode);
    change->set_node_id(2);
    std::ignore = raw_node.ApplyConfChange(cc);

    // Entries are not sent due to the node is removed.
    storage->TriggerLogUnavailable(false);
    auto context = storage->TakeGetEntriesContext();
    CHECK(context.has_value());
    raw_node.OnEntriesFetched(context.value());

    auto rd = raw_node.GetReady();
    // No messages to removed node
    auto msgs = rd.Messages();
    for (const auto& msg : msgs) {
        CHECK_NE(msg.to(), 2);
    }
    raw_node.AdvanceAppend(rd);
}

TEST_SUITE_END();
