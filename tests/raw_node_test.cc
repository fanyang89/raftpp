#include "raftpp/raw_node.h"

#include <doctest/doctest.h>

#include "harness/test_util.h"
#include "raftpp/error.h"
#include "raftpp/memory_storage.h"
#include "raftpp/raft_config.h"

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
            std::ignore = raw_node.Advance(rd);
            break;
        }
        std::ignore = raw_node.Advance(rd);
    }

    auto result = raw_node.Propose("", "testdata");
    CHECK(result);

    auto rd = raw_node.GetReady();
    CHECK(rd.entries.size() >= 1);
    CHECK_EQ(rd.entries.back().data(), "testdata");
    std::ignore = raw_node.Advance(rd);
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
    std::ignore = raw_node.Advance(rd);

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
    std::ignore = raw_node.AdvanceAppend(rd);

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
    std::ignore = raw_node.AdvanceAppend(rd);

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
    std::ignore = raw_node.AdvanceAppend(rd);
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
    std::ignore = raw_node.AdvanceAppend(rd);
}

/// Test that RawNode::step ignores local message.
TEST_CASE("raw_node: step local message ignored") {
    const std::vector<MessageType> local_msg_types{
        MsgHup, MsgBeat, MsgUnreachable, MsgSnapStatus, MsgCheckQuorum,
    };

    for (const auto msg_t : local_msg_types) {
        auto storage = std::make_shared<MemoryStorage>();
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
        snap.mutable_metadata()->mutable_conf_state()->add_voters(1);
        const auto snap_res = storage->ApplySnapshot(snap);
        REQUIRE(snap_res);

        Config config = DefaultConfig();
        config.id = 1;
        config.election_tick = 10;
        config.heartbeat_tick = 1;

        RawNode raw_node(config, storage);

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

/// Test that RawNode.read_index sends MsgReadIndex and ReadState can be read out.
/// TODO: This test may need adjustment based on raftpp's read index implementation
TEST_CASE("raw_node: read index") {
    const std::string request_ctx = "somedata";
    const std::vector<ReadState> wrs = {ReadState{2, request_ctx}};

    auto storage = std::make_shared<MemoryStorage>();
    // Initialize with snapshot like raft-rs does
    storage->ApplySnapshot(NewSnapshot(1, 1, {1})).value();

    RawNode raw_node = NewRawNode(1, {}, 10, 1, storage);

    raw_node.Campaign().value();

    while (true) {
        auto rd = raw_node.GetReady();
        storage->Append(rd.entries).value();
        if (rd.ss.has_value() && rd.ss->leader_id == 1) {
            std::ignore = raw_node.Advance(rd);

            // Once we are the leader, issue a read index request
            raw_node.ReadIndex(request_ctx);
            break;
        }
        std::ignore = raw_node.Advance(rd);
    }

    // Ensure read_states can be read out
    CHECK(raw_node.HasReady());
    auto rd = raw_node.GetReady();
    CHECK_EQ(rd.read_states, wrs);
    storage->Append(rd.entries).value();
    std::ignore = raw_node.Advance(rd);

    // Ensure raft.read_states is reset after advance
    CHECK_FALSE(raw_node.HasReady());
}

/// Test that a node can be started correctly.
TEST_CASE("raw_node: start") {
    auto storage = std::make_shared<MemoryStorage>();
    // Initialize with snapshot like raft-rs does
    storage->ApplySnapshot(NewSnapshot(1, 1, {1})).value();

    RawNode raw_node = NewRawNode(1, {}, 10, 1, storage);

    auto rd = raw_node.GetReady();
    MustCmpReady(rd, std::nullopt, std::nullopt, {}, {}, std::nullopt, true, true, false);
    std::ignore = raw_node.Advance(rd);

    raw_node.Campaign().value();
    auto rd2 = raw_node.GetReady();
    // NewEntry(index, term) - raft-rs: new_entry(term=2, index=2)
    // MakeHardState(term, commit, vote) - raft-rs: hard_state(term=2, commit=1, vote=1)
    MustCmpReady(
        rd2, std::make_optional(MakeSoftState(1, StateRole::Leader)),
        std::make_optional(MakeHardState(2, 1, 1)), {NewEntry(2, 2)}, {}, std::nullopt, true, true,
        true
    );
    storage->Append(rd2.entries).value();
    auto light_rd = raw_node.Advance(rd2);
    CHECK_EQ(light_rd.commit_index, std::make_optional<uint64_t>(2));
    CHECK_EQ(light_rd.committed_entries, std::vector<Entry>{NewEntry(2, 2)});
    CHECK_FALSE(raw_node.HasReady());

    raw_node.Propose("", "somedata").value();
    auto rd3 = raw_node.GetReady();
    // NewEntry(index, term, data) - raft-rs: new_entry(term=2, index=3, data)
    MustCmpReady(
        rd3, std::nullopt, std::nullopt, {NewEntry(3, 2, "somedata")}, {}, std::nullopt, true, true,
        true
    );
    storage->Append(rd3.entries).value();
    auto light_rd2 = raw_node.Advance(rd3);
    CHECK_EQ(light_rd2.commit_index, std::make_optional<uint64_t>(3));
    CHECK_EQ(light_rd2.committed_entries, std::vector<Entry>{NewEntry(3, 2, "somedata")});

    CHECK_FALSE(raw_node.HasReady());
}

/// Test node restart.
/// TODO: This test may need adjustment - raftpp may have different behavior for committed entries on restart
TEST_CASE("raw_node: restart") {
    // raft-rs: empty_entry(term=1, index=1), new_entry(term=1, index=2)
    // raftpp: EmptyEntry(index, term), NewEntry(index, term)
    const std::vector<Entry> entries = {EmptyEntry(1, 1), NewEntry(2, 1, "foo")};

    auto storage = std::make_shared<MemoryStorage>();
    HardState hs;
    hs.set_term(1);
    hs.set_vote(0);
    hs.set_commit(1);
    storage->SetRaftState({hs, {}});

    storage->Append(entries).value();

    Config config = DefaultConfig();
    config.id = 1;
    config.election_tick = 10;
    config.heartbeat_tick = 1;
    config.load_state_on_startup = true;

    RawNode raw_node(config, storage);

    auto rd = raw_node.GetReady();
    // After restart, committed entries up to commit index (1) should be returned
    MustCmpReady(
        rd, std::nullopt, std::nullopt, {}, {EmptyEntry(1, 1)}, std::nullopt, true, true, false
    );
    std::ignore = raw_node.Advance(rd);
    CHECK_FALSE(raw_node.HasReady());
}

/// Test node restart from snapshot.
/// TODO: This test may need adjustment - raftpp may have different behavior for committed entries on restart
TEST_CASE("raw_node: restart from snapshot") {
    // raft-rs: new_snapshot(index=2, term=1, voters)
    // raftpp: NewSnapshot(index, term, voters) ✓
    auto snap = NewSnapshot(2, 1, {1, 2});
    // raft-rs: new_entry(term=1, index=3, data)
    // raftpp: NewEntry(index, term, data)
    const std::vector<Entry> entries = {NewEntry(3, 1, "foo")};

    auto storage = std::make_shared<MemoryStorage>();
    storage->ApplySnapshot(snap).value();
    storage->Append(entries).value();

    HardState hs;
    hs.set_term(1);
    hs.set_commit(3);
    hs.set_vote(0);
    storage->SetRaftState({hs, {}});

    Config config = DefaultConfig();
    config.id = 1;
    config.election_tick = 10;
    config.heartbeat_tick = 1;
    config.load_state_on_startup = true;

    RawNode raw_node(config, storage);

    auto rd = raw_node.GetReady();
    MustCmpReady(rd, std::nullopt, std::nullopt, {}, entries, std::nullopt, true, true, false);
    std::ignore = raw_node.Advance(rd);
    CHECK_FALSE(raw_node.HasReady());
}

/// Test set priority function in RawNode.
TEST_CASE("raw_node: set priority") {
    auto storage = std::make_shared<MemoryStorage>();
    storage->ApplySnapshot(NewSnapshot(1, 1, {1})).value();

    RawNode raw_node = NewRawNode(1, {}, 10, 1, storage);

    const std::vector<int64_t> priorities = {0, 1, 5, 10, 10000};
    for (const auto p : priorities) {
        raw_node.SetPriority(p);
    }
}

/// Helper function to convert ConfChange to ConfChangeV2
static ConfChangeV2 ToConfChangeV2(const ConfChange& cc) {
    ConfChangeV2 cc_v2;
    auto* change = cc_v2.add_changes();
    change->set_change_type(cc.change_type());
    change->set_node_id(cc.node_id());
    return cc_v2;
}

/// Test that two proposes to add the same node should not affect the later propose
/// to add new node.
TEST_CASE("raw_node: propose add duplicate node") {
    auto storage = std::make_shared<MemoryStorage>();
    storage->ApplySnapshot(NewSnapshot(1, 1, {1})).value();

    RawNode raw_node = NewRawNode(1, {}, 10, 1, storage);
    raw_node.Campaign().value();

    while (true) {
        auto rd = raw_node.GetReady();
        storage->Append(rd.entries).value();
        if (rd.ss.has_value() && rd.ss->leader_id == 1) {
            std::ignore = raw_node.Advance(rd);
            break;
        }
        std::ignore = raw_node.Advance(rd);
    }

    auto propose_conf_change_and_apply = [&](const ConfChange& cc) {
        ConfChangeV2 cc_v2 = ToConfChangeV2(cc);
        raw_node.ProposeConfChange("", cc_v2).value();

        auto rd = raw_node.GetReady();
        storage->Append(rd.entries).value();

        auto handle_committed_entries = [&](const std::vector<Entry>& committed_entries) {
            for (const auto& e : committed_entries) {
                if (e.entry_type() == EntryConfChangeV2) {
                    ConfChangeV2 parsed_cc;
                    parsed_cc.ParseFromString(e.data());
                    raw_node.ApplyConfChange(parsed_cc).value();
                }
            }
        };

        handle_committed_entries(rd.light.committed_entries);

        auto light_rd = raw_node.Advance(rd);
        handle_committed_entries(light_rd.committed_entries);
        raw_node.AdvanceApply();
    };

    ConfChange cc1;
    cc1.set_change_type(ConfChangeType::AddNode);
    cc1.set_node_id(1);
    propose_conf_change_and_apply(cc1);

    // Try to add the same node again
    propose_conf_change_and_apply(cc1);

    // The new node join should be ok
    ConfChange cc2;
    cc2.set_change_type(ConfChangeType::AddNode);
    cc2.set_node_id(2);
    propose_conf_change_and_apply(cc2);
}

/// Test propose add learner node and check apply state.
TEST_CASE("raw_node: propose add learner node") {
    auto storage = std::make_shared<MemoryStorage>();
    storage->ApplySnapshot(NewSnapshot(1, 1, {1})).value();

    RawNode raw_node = NewRawNode(1, {}, 10, 1, storage);

    auto rd = raw_node.GetReady();
    MustCmpReady(rd, std::nullopt, std::nullopt, {}, {}, std::nullopt, true, true, false);
    std::ignore = raw_node.Advance(rd);

    raw_node.Campaign().value();
    while (true) {
        auto rd2 = raw_node.GetReady();
        storage->Append(rd2.entries).value();
        if (rd2.ss.has_value() && rd2.ss->leader_id == 1) {
            std::ignore = raw_node.Advance(rd2);
            break;
        }
        std::ignore = raw_node.Advance(rd2);
    }

    // Propose add learner node and check apply state
    ConfChange cc;
    cc.set_change_type(ConfChangeType::AddLearnerNode);
    cc.set_node_id(2);
    ConfChangeV2 cc_v2 = ToConfChangeV2(cc);
    raw_node.ProposeConfChange("", cc_v2).value();

    auto rd3 = raw_node.GetReady();
    storage->Append(rd3.entries).value();

    auto light_rd = raw_node.Advance(rd3);

    CHECK_GE(light_rd.committed_entries.size(), 1);

    const auto& e = light_rd.committed_entries[0];
    CHECK_EQ(e.entry_type(), EntryConfChangeV2);

    ConfChangeV2 parsed_cc;
    parsed_cc.ParseFromString(e.data());
    auto conf_state_result = raw_node.ApplyConfChange(parsed_cc).value();

    CHECK_EQ(conf_state_result.voters_size(), 1);
    CHECK_EQ(conf_state_result.voters(0), 1);
    CHECK_EQ(conf_state_result.learners_size(), 1);
    CHECK_EQ(conf_state_result.learners(0), 2);
}

/// Test that MsgReadIndex to old leader gets forwarded to the new leader.
/// This test verifies that followers forward MsgReadIndex to the leader,
/// and that an old leader (now follower) forwards pending MsgReadIndex to the new leader.
TEST_CASE("raw_node: read index to old leader") {
    const std::string request_ctx = "testdata";

    auto storage1 = std::make_shared<MemoryStorage>();
    auto storage2 = std::make_shared<MemoryStorage>();
    auto storage3 = std::make_shared<MemoryStorage>();

    Config config = DefaultConfig();
    config.election_tick = 10;
    config.heartbeat_tick = 1;

    // Create three nodes - set up config state for each
    ConfState cs;
    cs.add_voters(1);
    cs.add_voters(2);
    cs.add_voters(3);
    HardState hs;
    hs.set_commit(0);
    storage1->SetRaftState({hs, cs});
    storage2->SetRaftState({hs, cs});
    storage3->SetRaftState({hs, cs});

    config.id = 1;
    auto r1 = std::make_unique<Raft>(config, storage1);
    config.id = 2;
    auto r2 = std::make_unique<Raft>(config, storage2);
    config.id = 3;
    auto r3 = std::make_unique<Raft>(config, storage3);

    std::vector<std::unique_ptr<Interface>> ifaces;
    ifaces.push_back(std::make_unique<Interface>(std::move(r1), storage1));
    ifaces.push_back(std::make_unique<Interface>(std::move(r2), storage2));
    ifaces.push_back(std::make_unique<Interface>(std::move(r3), storage3));

    Network network = Network::Create(std::move(ifaces));

    // Elect r1 as leader
    network.Send({NewMessage(1, 1, MessageType::MsgHup)});

    // Create test entry with request context
    Entry test_entry;
    test_entry.set_data(request_ctx);

    // Send read index request to r2 (follower) using Step directly (not Send)
    // so messages stay in msgs() for inspection
    auto msg_to_r2 = NewMessageWithEntries(2, 2, MessageType::MsgReadIndex, {test_entry});
    network.GetPeer(2)->Step(msg_to_r2).value();

    // Verify r2 forwards to r1 (current leader) with term not set
    CHECK_EQ(network.GetPeer(2)->msgs().size(), 1);
    CHECK_EQ(network.GetPeer(2)->msgs()[0].msg_type(), MessageType::MsgReadIndex);
    CHECK_EQ(network.GetPeer(2)->msgs()[0].to(), 1);

    // Save this message for later
    auto read_index_msg1 = network.GetPeer(2)->msgs()[0];

    // Send read index request to r3 (follower) using Step directly
    auto msg_to_r3 = NewMessageWithEntries(3, 3, MessageType::MsgReadIndex, {test_entry});
    network.GetPeer(3)->Step(msg_to_r3).value();

    // Verify r3 forwards to r1 as well with term not set
    CHECK_EQ(network.GetPeer(3)->msgs().size(), 1);
    CHECK_EQ(network.GetPeer(3)->msgs()[0].msg_type(), MessageType::MsgReadIndex);
    CHECK_EQ(network.GetPeer(3)->msgs()[0].to(), 1);

    // Save this message for later
    auto read_index_msg2 = network.GetPeer(3)->msgs()[0];

    // Now elect r3 as new leader
    network.Send({NewMessage(3, 3, MessageType::MsgHup)});

    // Let r1 step the two messages previously from r2, r3
    // r1 is now a follower, so it should forward these to the new leader (r3)
    network.GetPeer(1)->Step(read_index_msg1).value();
    network.GetPeer(1)->Step(read_index_msg2).value();

    // Verify r1 (now follower) forwards these messages again to r3 (new leader)
    CHECK_EQ(network.GetPeer(1)->msgs().size(), 2);
    CHECK_EQ(network.GetPeer(1)->msgs()[0].msg_type(), MessageType::MsgReadIndex);
    CHECK_EQ(network.GetPeer(1)->msgs()[0].to(), 3);
    CHECK_EQ(network.GetPeer(1)->msgs()[1].msg_type(), MessageType::MsgReadIndex);
    CHECK_EQ(network.GetPeer(1)->msgs()[1].to(), 3);
}

/// Test configuration change mechanism.
TEST_CASE("raw_node: propose and conf change - simple add node") {
    auto storage = std::make_shared<MemoryStorage>();
    storage->ApplySnapshot(NewSnapshot(1, 1, {1})).value();

    RawNode raw_node = NewRawNode(1, {}, 10, 1, storage);

    raw_node.Campaign().value();

    bool proposed = false;
    ConfChange cc;
    cc.set_change_type(ConfChangeType::AddNode);
    cc.set_node_id(2);
    ConfChangeV2 cc_v2 = ToConfChangeV2(cc);
    std::string ccdata = cc_v2.SerializeAsString();

    std::optional<ConfState> cs;

    while (!cs.has_value()) {
        auto rd = raw_node.GetReady();
        storage->Append(rd.entries).value();

        auto handle_committed_entries = [&](const std::vector<Entry>& committed_entries) {
            for (const auto& e : committed_entries) {
                if (e.entry_type() == EntryConfChangeV2) {
                    ConfChangeV2 parsed_cc;
                    parsed_cc.ParseFromString(e.data());
                    cs = raw_node.ApplyConfChange(parsed_cc).value();
                }
            }
        };

        handle_committed_entries(rd.light.committed_entries);

        auto light_rd = raw_node.Advance(rd);
        handle_committed_entries(light_rd.committed_entries);
        raw_node.AdvanceApply();

        bool is_leader = rd.ss.has_value() && rd.ss->leader_id == raw_node.GetStatus().id;

        // Once we are the leader, propose a command and a ConfChange
        if (!proposed && is_leader) {
            raw_node.Propose("", "somedata").value();
            raw_node.ProposeConfChange("", cc_v2).value();
            proposed = true;
        }
    }

    // Check that the last index is exactly the conf change we put in
    const auto last_index = storage->LastIndex().value();
    auto entries =
        storage
            ->Entries(last_index - 1, last_index + 1, std::nullopt, GetEntriesContext::Empty(false))
            .value();
    CHECK_EQ(entries.size(), 2);
    CHECK_EQ(entries[0].data(), "somedata");
    CHECK_EQ(entries[1].entry_type(), EntryConfChangeV2);
    CHECK_EQ(entries[1].data(), ccdata);
    CHECK_EQ(cs.value(), MakeConfState({1, 2}));
}

/// Test configuration change mechanism - add learner node.
TEST_CASE("raw_node: propose and conf change - add learner") {
    auto storage = std::make_shared<MemoryStorage>();
    storage->ApplySnapshot(NewSnapshot(1, 1, {1})).value();

    RawNode raw_node = NewRawNode(1, {}, 10, 1, storage);

    raw_node.Campaign().value();

    while (true) {
        auto rd = raw_node.GetReady();
        storage->Append(rd.entries).value();

        auto handle_committed_entries = [&](const std::vector<Entry>& committed_entries) {
            for (const auto& e : committed_entries) {
                if (e.entry_type() == EntryConfChangeV2) {
                    ConfChangeV2 parsed_cc;
                    parsed_cc.ParseFromString(e.data());
                    raw_node.ApplyConfChange(parsed_cc).value();
                }
            }
        };

        handle_committed_entries(rd.light.committed_entries);

        auto light_rd = raw_node.Advance(rd);
        handle_committed_entries(light_rd.committed_entries);
        raw_node.AdvanceApply();

        bool is_leader = rd.ss.has_value() && rd.ss->leader_id == raw_node.GetStatus().id;

        // Once we are the leader, propose a command and a ConfChange
        if (is_leader) {
            raw_node.Propose("", "somedata").value();
            ConfChange cc;
            cc.set_change_type(ConfChangeType::AddLearnerNode);
            cc.set_node_id(2);
            ConfChangeV2 cc_v2 = ToConfChangeV2(cc);
            raw_node.ProposeConfChange("", cc_v2).value();
            break;
        }
    }

    // Process remaining ready to apply the conf change
    while (raw_node.HasReady()) {
        auto rd = raw_node.GetReady();
        storage->Append(rd.entries).value();

        for (const auto& e : rd.light.committed_entries) {
            if (e.entry_type() == EntryConfChangeV2) {
                ConfChangeV2 parsed_cc;
                parsed_cc.ParseFromString(e.data());
                raw_node.ApplyConfChange(parsed_cc).value();
            }
        }

        auto light_rd = raw_node.Advance(rd);
        for (const auto& e : light_rd.committed_entries) {
            if (e.entry_type() == EntryConfChangeV2) {
                ConfChangeV2 parsed_cc;
                parsed_cc.ParseFromString(e.data());
                raw_node.ApplyConfChange(parsed_cc).value();
            }
        }
        raw_node.AdvanceApply();
    }
}

/// Test configuration change auto leave even leader lost leadership.
TEST_CASE("raw_node: joint auto leave") {
    auto storage = std::make_shared<MemoryStorage>();
    storage->ApplySnapshot(NewSnapshot(1, 1, {1})).value();

    RawNode raw_node = NewRawNode(1, {}, 10, 1, storage);

    // Create joint configuration with auto leave
    ConfChangeV2 cc_v2;
    auto* change = cc_v2.add_changes();
    change->set_change_type(ConfChangeType::AddLearnerNode);
    change->set_node_id(2);
    cc_v2.set_transition(ConfChangeTransition::Implicit);
    std::string ccdata = cc_v2.SerializeAsString();

    // Campaign to become leader
    raw_node.Campaign().value();

    bool proposed = false;
    std::optional<ConfState> cs;

    // Propose ConfChange, wait until it applies, save resulting ConfState
    while (!cs.has_value()) {
        auto rd = raw_node.GetReady();
        storage->Append(rd.entries).value();

        auto handle_committed_entries = [&](const std::vector<Entry>& committed_entries) {
            for (const auto& e : committed_entries) {
                if (e.entry_type() == EntryConfChangeV2) {
                    ConfChangeV2 parsed_cc;
                    parsed_cc.ParseFromString(e.data());

                    // Force it step down
                    Message msg;
                    msg.set_to(1);
                    msg.set_from(1);
                    msg.set_msg_type(MessageType::MsgHeartbeatResponse);
                    msg.set_term(raw_node.GetStatus().hs.term() + 1);
                    raw_node.Step(msg).value();

                    cs = raw_node.ApplyConfChange(parsed_cc).value();
                }
            }
        };

        handle_committed_entries(rd.light.committed_entries);

        auto light_rd = raw_node.Advance(rd);
        handle_committed_entries(light_rd.committed_entries);
        raw_node.AdvanceApply();

        bool is_leader = rd.ss.has_value() && rd.ss->leader_id == raw_node.GetStatus().id;

        // Once we are leader, propose a command and a ConfChange
        if (!proposed && is_leader) {
            raw_node.Propose("", "somedata").value();
            raw_node.ProposeConfChange("", cc_v2).value();
            proposed = true;
        }
    }

    // Check that last index is exactly conf change we put in
    auto last_index = storage->LastIndex().value();
    auto entries =
        storage
            ->Entries(last_index - 1, last_index + 1, std::nullopt, GetEntriesContext::Empty(false))
            .value();
    CHECK_EQ(entries.size(), 2);
    CHECK_EQ(entries[0].data(), "somedata");
    CHECK_EQ(entries[1].entry_type(), EntryConfChangeV2);
    CHECK_EQ(ccdata, entries[1].data());

    // Move RawNode along. It should not leave joint because it's follower.
    auto rd = raw_node.GetReady();
    CHECK(rd.entries.empty());
    std::ignore = raw_node.Advance(rd);

    // Make it leader again. It should leave joint automatically after moving apply index.
    raw_node.Campaign().value();
    auto rd2 = raw_node.GetReady();
    storage->Append(rd2.entries).value();
    std::ignore = raw_node.Advance(rd2);

    auto rd3 = raw_node.GetReady();
    storage->Append(rd3.entries).value();

    // Check that right ConfChange comes out.
    CHECK_EQ(rd3.entries.size(), 1);
    CHECK_EQ(rd3.entries[0].entry_type(), EntryConfChangeV2);

    ConfChangeV2 leave_cc;
    leave_cc.ParseFromString(rd3.entries[0].data());
    CHECK(leave_cc.context().empty());

    // Lie and pretend ConfChange applied.
    auto final_cs = raw_node.ApplyConfChange(leave_cc).value();
    CHECK_EQ(final_cs, MakeConfState({1}, {2}));
}

/// Test bounded uncommitted entries growth with partition.
/// Tests a scenario where a leader is partitioned from a quorum of nodes.
/// It verifies that the leader's log is protected from unbounded growth
/// even as new entries continue to be proposed.
/// This protection is provided by the max_uncommitted_size configuration.
TEST_CASE("raw_node: bounded_uncommitted_entries_growth_with_partition") {
    auto storage = std::make_shared<MemoryStorage>();
    storage->ApplySnapshot(NewSnapshot(1, 1, {1})).value();

    Config config = DefaultConfig();
    config.id = 1;
    config.max_uncommitted_size = 12;
    config.election_tick = 10;
    config.heartbeat_tick = 1;

    RawNode raw_node(config, storage);

    // Wait raw_node to be leader
    raw_node.Campaign().value();
    while (true) {
        auto rd = raw_node.GetReady();
        if (rd.hs.has_value()) {
            storage->SetRaftState({rd.hs.value(), {}});
        }
        storage->Append(rd.entries).value();
        if (rd.ss.has_value() && rd.ss->leader_id == raw_node.GetStatus().id) {
            std::ignore = raw_node.Advance(rd);
            break;
        }
        std::ignore = raw_node.Advance(rd);
    }

    // Should be accepted
    std::string data = "hello world!";
    raw_node.Propose("", data).value();

    // Should be dropped
    auto result = raw_node.Propose("", data);
    CHECK_FALSE(result.has_value());
    CHECK(result.error() == RaftErrorCode::ProposalDropped);

    // Should be accepted when previous data has been committed
    auto rd = raw_node.GetReady();
    storage->Append(rd.entries).value();
    std::ignore = raw_node.Advance(rd);

    raw_node.Propose("", data).value();
}

/// Test raw_node with async apply.
/// Tests incremental apply using AdvanceApplyTo method.
TEST_CASE("raw_node: with async apply") {
    auto storage = std::make_shared<MemoryStorage>();
    Snapshot snap = NewSnapshot(1, 1, {1});
    storage->ApplySnapshot(snap).value();

    Config config = DefaultConfig();
    config.id = 1;
    config.election_tick = 10;
    config.heartbeat_tick = 1;
    RawNode raw_node(config, storage);

    raw_node.Campaign().value();
    auto rd = raw_node.GetReady();
    // Single node should become leader.
    CHECK(rd.ss.has_value());
    CHECK_EQ(rd.ss->leader_id, raw_node.GetStatus().id);
    storage->Append(rd.entries).value();
    std::ignore = raw_node.Advance(rd);

    // raft-rs uses: raw_node.raft.raft_log.last_index()
    // In raftpp we get this from storage after appending
    uint64_t last_index = storage->LastIndex().value();

    std::string data = "hello world!";

    for (int i = 1; i < 10; ++i) {
        int cnt = (rand() % 10) + 1;
        for (int j = 0; j < cnt; ++j) {
            raw_node.Propose("", data).value();
        }

        auto rd2 = raw_node.GetReady();
        auto entries = rd2.entries;
        CHECK_EQ(entries[0].index(), last_index + 1);
        CHECK_EQ(entries[entries.size() - 1].index(), last_index + cnt);
        MustCmpReady(rd2, std::nullopt, std::nullopt, entries, {}, std::nullopt, true, true, true);

        storage->Append(entries).value();

        auto light_rd = raw_node.Advance(rd2);
        CHECK_EQ(light_rd.committed_entries, entries);
        CHECK(light_rd.commit_index.has_value());
        CHECK_EQ(light_rd.commit_index.value(), last_index + cnt);

        // No matter how applied index changes, the index of next committed
        // entries should be the same.
        raw_node.AdvanceApplyTo(last_index + 1);
        CHECK(!raw_node.HasReady());

        last_index += cnt;
    }
}

/// Test if the ready process is expected when a follower receives a snapshot
/// and some committed entries after its snapshot.
TEST_CASE("raw_node: entries_after_snapshot") {
    auto storage = std::make_shared<MemoryStorage>();
    storage->ApplySnapshot(NewSnapshot(1, 1, {1, 2})).value();

    Config config = DefaultConfig();
    config.id = 1;
    config.election_tick = 10;
    config.heartbeat_tick = 1;
    RawNode raw_node(config, storage);

    std::vector<Entry> entries;
    for (int i = 2; i < 20; ++i) {
        // NewEntry(index, term, data) - raft-rs: new_entry(term=2, index=i)
        entries.push_back(NewEntry(i, 2, "hello"));
    }
    Message append_msg = NewMessageWithEntries(2, 1, MessageType::MsgAppend, entries);
    append_msg.set_term(2);
    append_msg.set_index(1);
    append_msg.set_log_term(1);
    append_msg.set_commit(5);
    raw_node.Step(append_msg).value();

    auto rd = raw_node.GetReady();
    MustCmpReady(
        rd, std::make_optional(MakeSoftState(2, StateRole::Follower)),
        std::make_optional(MakeHardState(2, 5, 0)), entries, {}, std::nullopt, true, false, true
    );
    storage->SetRaftState({rd.hs.value(), {}});
    storage->Append(rd.entries).value();
    auto light_rd = raw_node.Advance(rd);
    CHECK(!light_rd.commit_index.has_value());
    CHECK_EQ(light_rd.committed_entries, std::vector<Entry>(entries.begin(), entries.begin() + 4));
    CHECK(light_rd.messages.empty());

    Snapshot snap = NewSnapshot(10, 3, {1, 2});
    Message snapshot_msg = NewMessage(2, 1, MessageType::MsgSnapshot, 0);
    snapshot_msg.set_term(3);
    *snapshot_msg.mutable_snapshot() = snap;
    raw_node.Step(snapshot_msg).value();

    entries.clear();
    for (int i = 11; i < 14; ++i) {
        // NewEntry(index, term, data) - raft-rs: new_entry(term=3, index=i)
        entries.push_back(NewEntry(i, 3, "hello"));
    }
    append_msg = NewMessageWithEntries(2, 1, MessageType::MsgAppend, entries);
    append_msg.set_term(3);
    append_msg.set_index(10);
    append_msg.set_log_term(3);
    append_msg.set_commit(12);
    raw_node.Step(append_msg).value();

    auto rd2 = raw_node.GetReady();
    // If there is a snapshot, the committed entries should be empty.
    MustCmpReady(
        rd2, std::nullopt, std::make_optional(MakeHardState(3, 12, 0)), entries, {},
        std::make_optional(snap), true, false, true
    );
    // Should have a MsgAppendResponse
    CHECK_EQ(rd2.light.messages[0].msg_type(), MessageType::MsgAppendResponse);
    storage->SetRaftState({rd2.hs.value(), {}});
    storage->ApplySnapshot(rd2.snapshot).value();
    storage->Append(rd2.entries).value();

    auto light_rd2 = raw_node.Advance(rd2);
    CHECK(!light_rd2.commit_index.has_value());
    CHECK_EQ(light_rd2.committed_entries, std::vector<Entry>(entries.begin(), entries.begin() + 2));
    CHECK(light_rd2.messages.empty());
}

/// Test if the given committed entries are persisted when some persisted
/// entries are overwritten by a new leader.
TEST_CASE("raw_node: overwrite_entries") {
    auto storage = std::make_shared<MemoryStorage>();
    storage->ApplySnapshot(NewSnapshot(1, 1, {1, 2, 3})).value();

    Config config = DefaultConfig();
    config.id = 1;
    config.election_tick = 10;
    config.heartbeat_tick = 1;
    RawNode raw_node(config, storage);

    // NewEntry(index, term, data) - raft-rs: new_entry(term=2, index=2..4)
    std::vector<Entry> entries = {
        NewEntry(2, 2, "hello"),
        NewEntry(3, 2, "hello"),
        NewEntry(4, 2, "hello"),
    };
    Message append_msg = NewMessageWithEntries(2, 1, MessageType::MsgAppend, entries);
    append_msg.set_term(2);
    append_msg.set_index(1);
    append_msg.set_log_term(1);
    append_msg.set_commit(1);
    raw_node.Step(append_msg).value();

    auto rd = raw_node.GetReady();
    MustCmpReady(
        rd, std::make_optional(MakeSoftState(2, StateRole::Follower)),
        std::make_optional(MakeHardState(2, 1, 0)), entries, {}, std::nullopt, true, false, true
    );
    // Should have a MsgAppendResponse
    CHECK_EQ(rd.light.messages[0].msg_type(), MessageType::MsgAppendResponse);
    storage->SetRaftState({rd.hs.value(), {}});
    storage->Append(rd.entries).value();

    auto light_rd = raw_node.Advance(rd);
    CHECK(!light_rd.commit_index.has_value());
    CHECK(light_rd.committed_entries.empty());
    CHECK(light_rd.messages.empty());

    // NewEntry(index, term, data) - raft-rs: new_entry(term=3, index=4..6)
    std::vector<Entry> entries_2 = {
        NewEntry(4, 3, "hello"),
        NewEntry(5, 3, "hello"),
        NewEntry(6, 3, "hello"),
    };
    append_msg = NewMessageWithEntries(3, 1, MessageType::MsgAppend, entries_2);
    append_msg.set_term(3);
    append_msg.set_index(3);
    append_msg.set_log_term(2);
    append_msg.set_commit(5);
    raw_node.Step(append_msg).value();

    auto rd2 = raw_node.GetReady();
    MustCmpReady(
        rd2, std::make_optional(MakeSoftState(3, StateRole::Follower)),
        std::make_optional(MakeHardState(3, 5, 0)), entries_2,
        std::vector<Entry>(entries.begin(), entries.begin() + 2), std::nullopt, true, false, true
    );
    // Should have a MsgAppendResponse
    CHECK_EQ(rd2.light.messages[0].msg_type(), MessageType::MsgAppendResponse);
    storage->SetRaftState({rd2.hs.value(), {}});
    storage->Append(rd2.entries).value();

    auto light_rd2 = raw_node.Advance(rd2);
    CHECK(!light_rd2.commit_index.has_value());
    CHECK_EQ(
        light_rd2.committed_entries, std::vector<Entry>(entries_2.begin(), entries_2.begin() + 2)
    );
    CHECK(light_rd2.messages.empty());
}

/// Test committed entries pagination.
/// Tests the max_committed_size_per_ready configuration option.
TEST_CASE("raw_node: committed_entries_pagination") {
    auto storage = std::make_shared<MemoryStorage>();
    storage->ApplySnapshot(NewSnapshot(1, 1, {1, 2, 3})).value();

    RawNode raw_node = NewRawNode(1, {}, 10, 1, storage);

    std::vector<Entry> entries;
    for (int i = 2; i < 10; ++i) {
        // EmptyEntry(index, term) - raft-rs: empty_entry(term=1, index=i)
        entries.push_back(EmptyEntry(i, 1));
    }
    Message msg = NewMessageWithEntries(3, 1, MessageType::MsgAppend, entries);
    msg.set_term(1);
    msg.set_index(1);
    msg.set_log_term(1);
    msg.set_commit(9);
    raw_node.Step(msg).value();

    // Test unpersisted entries won't be fetched.
    auto rd = raw_node.GetReady();
    CHECK(rd.light.committed_entries.empty());
    CHECK(raw_node.HasReady());

    // Persist entries.
    CHECK(!rd.entries.empty());
    storage->Append(rd.entries).value();

    // Advance the ready, and we can get committed_entries as expected.
    auto light_rd = raw_node.Advance(rd);
    CHECK_EQ(light_rd.committed_entries.size(), 8);

    // No more `Ready`s.
    CHECK(!raw_node.HasReady());
}

/// Test disable proposal forwarding.
/// Tests that when disable_proposal_forwarding is true,
/// proposals to followers are dropped instead of forwarded to the leader.
TEST_CASE("raw_node: disable_proposal_forwarding") {
    auto storage1 = std::make_shared<MemoryStorage>();
    storage1->ApplySnapshot(NewSnapshot(1, 1, {1, 2, 3})).value();

    Config config1 = DefaultConfig();
    config1.id = 1;
    config1.heartbeat_tick = 1;
    config1.election_tick = 10;
    config1.disable_proposal_forwarding = false;
    auto r1 = std::make_unique<Raft>(config1, storage1);

    auto storage2 = std::make_shared<MemoryStorage>();
    storage2->ApplySnapshot(NewSnapshot(1, 1, {1, 2, 3})).value();
    Config config2 = DefaultConfig();
    config2.id = 2;
    config2.heartbeat_tick = 1;
    config2.election_tick = 10;
    config2.disable_proposal_forwarding = false;
    auto r2 = std::make_unique<Raft>(config2, storage2);

    auto storage3 = std::make_shared<MemoryStorage>();
    storage3->ApplySnapshot(NewSnapshot(1, 1, {1, 2, 3})).value();
    Config config3 = DefaultConfig();
    config3.id = 3;
    config3.heartbeat_tick = 1;
    config3.election_tick = 10;
    config3.disable_proposal_forwarding = true;
    auto r3 = std::make_unique<Raft>(config3, storage3);

    std::vector<std::unique_ptr<Interface>> peers;
    peers.push_back(std::make_unique<Interface>(std::move(r1), storage1));
    peers.push_back(std::make_unique<Interface>(std::move(r2), storage2));
    peers.push_back(std::make_unique<Interface>(std::move(r3), storage3));
    Network nt = Network::Create(std::move(peers));

    // Node 1 starts campaign to become leader.
    nt.Send({NewMessage(1, 1, MessageType::MsgHup, 0)});

    // Send proposal to n2(follower) where DisableProposalForwarding is false
    auto msg = NewMessage(2, 2, MessageType::MsgPropose, 1);
    auto result = nt.GetPeer(2)->Step(msg);
    CHECK(result.has_value());

    // Verify n2(follower) does forward the proposal when DisableProposalForwarding is false
    CHECK_EQ(nt.GetPeer(2)->msgs().size(), 1);

    // Send proposal to n3(follower) where DisableProposalForwarding is true
    auto msg2 = NewMessage(3, 3, MessageType::MsgPropose, 1);
    auto result2 = nt.GetPeer(3)->Step(msg2);
    CHECK_FALSE(result2.has_value());
    CHECK(result2.error() == RaftErrorCode::ProposalDropped);

    CHECK(nt.GetPeer(3)->msgs().empty());
}

TEST_SUITE_END();
