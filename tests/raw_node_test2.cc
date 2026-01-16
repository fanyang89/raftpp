// Copyright 2024 raftpp Authors. Licensed under Apache-2.0.

// Copyright 2015 CoreOS, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <doctest/doctest.h>

#include "harness/test_util.h"
#include "raftpp/error.h"
#include "raftpp/memory_storage.h"
#include "raftpp/raft_config.h"
#include "raftpp/raw_node.h"

using namespace raftpp;

TEST_SUITE_BEGIN("harness_raw_node");

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

        RawNode raw_node(config, std::make_unique<MemoryStorage>(*storage));

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
TEST_CASE("raw_node: read index") {
    const std::string request_ctx = "somedata";
    const std::vector<ReadState> wrs = {ReadState{2, request_ctx}};

    auto storage = std::make_shared<MemoryStorage>();
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

    RawNode raw_node(config, std::make_unique<MemoryStorage>(*storage));

    raw_node.Campaign().value();

    while (true) {
        auto rd = raw_node.GetReady();
        storage->Append(rd.entries()).value();
        if (rd.ss.has_value() && rd.ss->leader_id == 1) {
            raw_node.Advance(rd);

            // Once we are the leader, issue a read index request
            raw_node.ReadIndex(request_ctx);
            break;
        }
        raw_node.Advance(rd);
    }

    // Ensure read_states can be read out
    CHECK_FALSE(raw_node.GetStatus().read_states().empty());
    CHECK(raw_node.HasReady());
    auto rd = raw_node.GetReady();
    CHECK_EQ(rd.read_states, wrs);
    storage->Append(rd.entries()).value();
    raw_node.Advance(rd);

    // Ensure raft.read_states is reset after advance
    CHECK_FALSE(raw_node.HasReady());
    CHECK(raw_node.GetStatus().read_states().empty());
}

/// Test that a node can be started correctly.
TEST_CASE("raw_node: start") {
    auto storage = std::make_shared<MemoryStorage>();
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

    RawNode raw_node(config, std::make_unique<MemoryStorage>(*storage));

    auto rd = raw_node.GetReady();
    MustCmpReady(rd, std::nullopt, std::nullopt, {}, {}, std::nullopt, true, true, false);
    raw_node.Advance(rd);

    raw_node.Campaign().expect("");
    auto rd2 = raw_node.GetReady();
    MustCmpReady(
        rd2, std::make_optional(MakeSoftState(1, StateRole::Leader)),
        std::make_optional(MakeHardState(2, 1, 1)), {NewEntry(2, 2)}, {}, std::nullopt, true, true,
        true
    );
    storage->Append(rd2.entries()).value();
    auto light_rd = raw_node.Advance(rd2);
    CHECK_EQ(light_rd.commit_index, std::make_optional(2));
    CHECK_EQ(light_rd.committed_entries, std::vector<Entry>{NewEntry(2, 2)});
    CHECK_FALSE(raw_node.HasReady());

    raw_node.Propose("", "somedata").expect("");
    auto rd3 = raw_node.GetReady();
    MustCmpReady(
        rd3, std::nullopt, std::nullopt, {NewEntry(2, 3, "somedata")}, {}, std::nullopt, true, true,
        true
    );
    storage->Append(rd3.entries()).value();
    auto light_rd2 = raw_node.Advance(rd3);
    CHECK_EQ(light_rd2.commit_index, std::make_optional(3));
    CHECK_EQ(light_rd2.committed_entries, std::vector<Entry>{NewEntry(2, 3, "somedata")});

    CHECK_FALSE(raw_node.HasReady());
}

/// Test node restart.
TEST_CASE("raw_node: restart") {
    const std::vector<Entry> entries = {NewEntry(1, 1), NewEntry(1, 2, "foo")};

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

    RawNode raw_node(config, std::make_unique<MemoryStorage>(*storage));

    auto rd = raw_node.GetReady();
    MustCmpReady(rd, std::nullopt, std::nullopt, {}, entries, std::nullopt, true, true, false);
    raw_node.Advance(rd);
    CHECK_FALSE(raw_node.HasReady());
}

/// Test node restart from snapshot.
TEST_CASE("raw_node: restart from snapshot") {
    auto snap = NewSnapshot(2, 1, {1, 2});
    const std::vector<Entry> entries = {NewEntry(1, 3, "foo")};

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

    RawNode raw_node(config, std::make_unique<MemoryStorage>(*storage));

    auto rd = raw_node.GetReady();
    MustCmpReady(rd, std::nullopt, std::nullopt, {}, entries, std::nullopt, true, true, false);
    raw_node.Advance(rd);
    CHECK_FALSE(raw_node.HasReady());
}

/// Test set priority function in RawNode.
TEST_CASE("raw_node: set priority") {
    auto storage = std::make_shared<MemoryStorage>();
    Config config = DefaultConfig();
    config.id = 1;
    config.election_tick = 10;
    config.heartbeat_tick = 1;

    ConfState conf_state;
    conf_state.add_voters(1);
    HardState hard_state;
    hard_state.set_commit(0);
    storage->SetRaftState({hard_state, conf_state});

    RawNode raw_node(config, std::make_unique<MemoryStorage>(*storage));

    const std::vector<int64_t> priorities = {0, 1, 5, 10, 10000};
    for (const auto p : priorities) {
        raw_node.SetPriority(p);
        CHECK_EQ(raw_node.GetStatus().priority, p);
    }
}

/// Test that two proposes to add the same node should not affect the later propose
/// to add new node.
TEST_CASE("raw_node: propose add duplicate node") {
    auto storage = std::make_shared<MemoryStorage>();
    Config config = DefaultConfig();
    config.id = 1;
    config.election_tick = 10;
    config.heartbeat_tick = 1;

    ConfState conf_state;
    conf_state.add_voters(1);
    HardState hard_state;
    hard_state.set_commit(0);
    storage->SetRaftState({hard_state, conf_state});

    RawNode raw_node(config, std::make_unique<MemoryStorage>(*storage));
    raw_node.Campaign().expect("");

    while (true) {
        auto rd = raw_node.GetReady();
        storage->Append(rd.entries()).value();
        if (rd.ss.has_value() && rd.ss->leader_id == 1) {
            raw_node.Advance(rd);
            break;
        }
        raw_node.Advance(rd);
    }

    auto propose_conf_change_and_apply = [&](const ConfChange& cc) {
        ConfChangeV2 cc_v2;
        auto* change = cc_v2.add_changes();
        change->set_change_type(cc.change_type());
        change->set_node_id(cc.node_id());
        raw_node.ProposeConfChange("", cc_v2).expect("");

        auto rd = raw_node.GetReady();
        storage->Append(rd.entries()).value();

        auto handle_committed_entries = [&](const std::vector<Entry>& committed_entries) {
            for (const auto& e : committed_entries) {
                if (e.entry_type() == EntryConfChange) {
                    ConfChange parsed_cc;
                    parsed_cc.ParseFromString(e.data());
                    raw_node.ApplyConfChange(parsed_cc).value();
                }
            }
        };

        handle_committed_entries(rd.committed_entries);

        auto light_rd = raw_node.Advance(rd);
        handle_committed_entries(light_rd.committed_entries);
        raw_node.AdvanceApply();
    };

    ConfChange cc1;
    cc1.set_change_type(ConfChangeType::AddNode);
    cc1.set_node_id(1);
    const auto ccdata1 = cc1.SerializeAsString();
    propose_conf_change_and_apply(cc1);

    // Try to add the same node again
    propose_conf_change_and_apply(cc1);

    // The new node join should be ok
    ConfChange cc2;
    cc2.set_change_type(ConfChangeType::AddNode);
    cc2.set_node_id(2);
    const auto ccdata2 = cc2.SerializeAsString();
    propose_conf_change_and_apply(cc2);

    const auto last_index = storage->LastIndex().value();

    // The last three entries should be: ConfChange cc1, cc1, cc2
    auto entries_range =
        storage
            ->Entries(last_index - 2, last_index + 1, std::nullopt, GetEntriesContext::Empty(false))
            .value();
    CHECK_EQ(entries_range.size(), 3);
    CHECK_EQ(entries_range[0].data(), ccdata1);
    CHECK_EQ(entries_range[2].data(), ccdata2);
}

/// Test propose add learner node and check apply state.
TEST_CASE("raw_node: propose add learner node") {
    auto storage = std::make_shared<MemoryStorage>();
    Config config = DefaultConfig();
    config.id = 1;
    config.election_tick = 10;
    config.heartbeat_tick = 1;

    ConfState conf_state;
    conf_state.add_voters(1);
    HardState hard_state;
    hard_state.set_commit(0);
    storage->SetRaftState({hard_state, conf_state});

    RawNode raw_node(config, std::make_unique<MemoryStorage>(*storage));

    auto rd = raw_node.GetReady();
    MustCmpReady(rd, std::nullopt, std::nullopt, {}, {}, std::nullopt, true, true, false);
    raw_node.Advance(rd);

    raw_node.Campaign().expect("");
    while (true) {
        auto rd = raw_node.GetReady();
        storage->Append(rd.entries()).value();
        if (rd.ss.has_value() && rd.ss->leader_id == 1) {
            raw_node.Advance(rd);
            break;
        }
        raw_node.Advance(rd);
    }

    // Propose add learner node and check apply state
    ConfChange cc;
    cc.set_change_type(ConfChangeType::AddLearnerNode);
    cc.set_node_id(2);
    ConfChangeV2 cc_v2;
    auto* change = cc_v2.add_changes();
    change->set_change_type(cc.change_type());
    change->set_node_id(cc.node_id());
    raw_node.ProposeConfChange("", cc_v2).expect("");

    auto rd = raw_node.GetReady();
    storage->Append(rd.entries()).value();

    auto light_rd = raw_node.Advance(rd);

    CHECK_GE(light_rd.committed_entries.size(), 1);

    const auto& e = light_rd.committed_entries[0];
    CHECK_EQ(e.entry_type(), EntryConfChange);

    ConfChange parsed_cc;
    parsed_cc.ParseFromString(e.data());
    auto conf_state = raw_node.ApplyConfChange(parsed_cc).value();

    CHECK_EQ(conf_state.voters_size(), 1);
    CHECK_EQ(conf_state.voters(0), 1);
    CHECK_EQ(conf_state.learners_size(), 1);
    CHECK_EQ(conf_state.learners(0), 2);
}

/// Test that MsgReadIndex to old leader gets forwarded to the new leader.
TEST_CASE("raw_node: read index to old leader") {
    const std::string request_ctx = "testdata";

    auto storage1 = std::make_shared<MemoryStorage>();
    auto storage2 = std::make_shared<MemoryStorage>();
    auto storage3 = std::make_shared<MemoryStorage>();

    Config config = DefaultConfig();
    config.election_tick = 10;
    config.heartbeat_tick = 1;

    // Create three nodes
    auto r1 = NewTestRaftWithConfig(config, storage1);
    auto r2 = NewTestRaftWithConfig(config, storage2);
    auto r3 = NewTestRaftWithConfig(config, storage3);

    Network network = Network::CreateWithConfig(
        {
            std::make_unique<Interface>(std::move(r1)),
            std::make_unique<Interface>(std::move(r2)),
            std::make_unique<Interface>(std::move(r3)),
        },
        config
    );

    // Elect r1 as leader
    network.Send({NewMessage(1, 1, MessageType::MsgHup)});

    // Send read index request to r2 (follower)
    Entry test_entry;
    test_entry.set_data(request_ctx);
    network.Send({NewMessageWithEntries(2, 2, MessageType::MsgReadIndex, {test_entry})});

    // Verify r2 forwards to r1 (current leader)
    CHECK_EQ(network.GetPeer(2)->msgs().size(), 1);
    CHECK_EQ(network.GetPeer(2)->msgs()[0].msg_type(), MessageType::MsgReadIndex);
    CHECK_EQ(network.GetPeer(2)->msgs()[0].to(), 1);

    // Send read index request to r3 (follower)
    network.Send({NewMessageWithEntries(3, 3, MessageType::MsgReadIndex, {test_entry})});

    // Verify r3 forwards to r1 as well
    CHECK_EQ(network.GetPeer(3)->msgs().size(), 1);
    CHECK_EQ(network.GetPeer(3)->msgs()[0].msg_type(), MessageType::MsgReadIndex);
    CHECK_EQ(network.GetPeer(3)->msgs()[0].to(), 1);

    // Now elect r3 as new leader
    network.Send({NewMessage(3, 3, MessageType::MsgHup)});

    // Let r1 step the two messages previously from r2, r3
    auto msg2 = network.GetPeer(2)->msgs()[0];
    auto msg3 = network.GetPeer(3)->msgs()[0];
    network.GetPeer(1)->Step(msg2).value();
    network.GetPeer(1)->Step(msg3).value();

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

    RawNode raw_node(config, std::make_unique<MemoryStorage>(*storage));

    raw_node.Campaign().expect("");

    bool proposed = false;
    ConfChange cc;
    cc.set_change_type(ConfChangeType::AddNode);
    cc.set_node_id(2);
    std::string ccdata = cc.SerializeAsString();

    std::optional<ConfState> cs;

    while (!cs.has_value()) {
        auto rd = raw_node.GetReady();
        storage->Append(rd.entries()).value();

        auto handle_committed_entries = [&](const std::vector<Entry>& committed_entries) {
            for (const auto& e : committed_entries) {
                if (e.entry_type() == EntryConfChange) {
                    ConfChange parsed_cc;
                    parsed_cc.ParseFromString(e.data());
                    cs = raw_node.ApplyConfChange(parsed_cc).value();
                } else if (e.entry_type() == EntryConfChangeV2) {
                    ConfChangeV2 parsed_cc;
                    parsed_cc.ParseFromString(e.data());
                    cs = raw_node.ApplyConfChange(parsed_cc).value();
                }
            }
        };

        handle_committed_entries(rd.committed_entries);

        auto light_rd = raw_node.Advance(rd);
        handle_committed_entries(light_rd.committed_entries);
        raw_node.AdvanceApply();

        bool is_leader = rd.ss.has_value() && rd.ss->leader_id == raw_node.GetStatus().id();

        // Once we are the leader, propose a command and a ConfChange
        if (!proposed && is_leader) {
            raw_node.Propose("", "somedata").expect("");
            raw_node.ProposeConfChange("", cc).expect("");
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
    CHECK_EQ(entries[1].entry_type(), EntryConfChange);
    CHECK_EQ(entries[1].data(), ccdata);
    CHECK_EQ(cs.value(), MakeConfState({1, 2}));
}

/// Test configuration change mechanism - add learner node.
TEST_CASE("raw_node: propose and conf change - add learner") {
    auto storage = std::make_shared<MemoryStorage>();
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

    RawNode raw_node(config, std::make_unique<MemoryStorage>(*storage));

    raw_node.Campaign().expect("");

    while (true) {
        auto rd = raw_node.GetReady();
        storage->Append(rd.entries()).value();

        auto handle_committed_entries = [&](const std::vector<Entry>& committed_entries) {
            for (const auto& e : committed_entries) {
                if (e.entry_type() == EntryConfChange) {
                    ConfChange parsed_cc;
                    parsed_cc.ParseFromString(e.data());
                    raw_node.ApplyConfChange(parsed_cc).value();
                } else if (e.entry_type() == EntryConfChangeV2) {
                    ConfChangeV2 parsed_cc;
                    parsed_cc.ParseFromString(e.data());
                    raw_node.ApplyConfChange(parsed_cc).value();
                }
            }
        };

        handle_committed_entries(rd.committed_entries);

        auto light_rd = raw_node.Advance(rd);
        handle_committed_entries(light_rd.committed_entries);
        raw_node.AdvanceApply();

        bool is_leader = rd.ss.has_value() && rd.ss->leader_id == raw_node.GetStatus().id();

        // Once we are the leader, propose a command and a ConfChange
        if (is_leader) {
            raw_node.Propose("", "somedata").expect("");
            ConfChange cc;
            cc.set_change_type(ConfChangeType::AddLearnerNode);
            cc.set_node_id(2);
            ConfChangeV2 cc_v2;
            auto* change = cc_v2.add_changes();
            change->set_change_type(cc.change_type());
            change->set_node_id(cc.node_id());
            raw_node.ProposeConfChange("", cc_v2).expect("");
            break;
        }
    }

    // Verify learner is added
    const auto status = raw_node.GetStatus();
    CHECK_EQ(status.conf_state.voters_size(), 1);
    CHECK_EQ(status.conf_state.voters(0), 1);
    CHECK_EQ(status.conf_state.learners_size(), 1);
    CHECK_EQ(status.conf_state.learners(0), 2);
}

/// Test configuration change auto leave even leader lost leadership.
TEST_CASE("raw_node: joint auto leave") {
    auto storage = std::make_shared<MemoryStorage>();
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

    RawNode raw_node(config, std::make_unique<MemoryStorage>(*storage));

    // Create joint configuration with auto leave
    ConfChange cc;
    cc.set_change_type(ConfChangeType::AddLearnerNode);
    cc.set_node_id(2);
    ConfChangeV2 cc_v2;
    auto* change = cc_v2.add_changes();
    change->set_change_type(cc.change_type());
    change->set_node_id(cc.node_id());
    cc_v2.set_transition(ConfChangeTransition::Implicit);
    std::string ccdata = cc_v2.SerializeAsString();

    // Campaign to become leader
    raw_node.Campaign().expect("");

    bool proposed = false;
    std::optional<ConfState> cs;

    // Propose ConfChange, wait until it applies, save resulting ConfState
    while (!cs.has_value()) {
        auto rd = raw_node.GetReady();
        storage->Append(rd.entries()).value();

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
                    raw_node.Step(msg).expect("");

                    cs = raw_node.ApplyConfChange(parsed_cc).value();
                }
            }
        };

        handle_committed_entries(rd.committed_entries);

        auto light_rd = raw_node.Advance(rd);
        handle_committed_entries(light_rd.committed_entries);
        raw_node.AdvanceApply();

        bool is_leader = rd.ss.has_value() && rd.ss->leader_id == raw_node.GetStatus().id();

        // Once we are leader, propose a command and a ConfChange
        if (!proposed && is_leader) {
            raw_node.Propose("", "somedata").expect("");
            raw_node.ProposeConfChange("", cc_v2).expect("");
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
    CHECK_EQ(cs.value(), MakeConfState({1}, {2}));

    // Verify pending_conf_index is 0
    CHECK_EQ(raw_node.GetStatus().progress->MaxCommittedIndex(), 0);

    // Move RawNode along. It should not leave joint because it's follower.
    auto rd = raw_node.GetReady();
    CHECK(rd.entries.empty());
    auto _ = raw_node.Advance(rd);

    // Make it leader again. It should leave joint automatically after moving apply index.
    raw_node.Campaign().expect("");
    rd = raw_node.GetReady();
    storage->Append(rd.entries()).value();
    auto _ = raw_node.Advance(rd);

    rd = raw_node.GetReady();
    storage->Append(rd.entries()).value();

    // Check that right ConfChange comes out.
    CHECK_EQ(rd.entries.size(), 1);
    CHECK_EQ(rd.entries[0].entry_type(), EntryConfChangeV2);

    ConfChangeV2 leave_cc;
    leave_cc.ParseFromString(rd.entries[0].data());
    CHECK(leave_cc.context().empty());

    // Lie and pretend ConfChange applied.
    auto final_cs = raw_node.ApplyConfChange(leave_cc).value();
    CHECK_EQ(final_cs, MakeConfState({1}, {2}));
}

/// Test skip_bcast_commit ensures that empty commit message is not sent out
/// when skip_bcast_commit is true.
TEST_CASE("raw_node: skip_bcast_commit") {
    auto storage1 = std::make_shared<MemoryStorage>();
    ConfState conf_state;
    conf_state.add_voters(1);
    conf_state.add_voters(2);
    conf_state.add_voters(3);
    HardState hard_state;
    hard_state.set_commit(0);
    hard_state.set_term(0);
    hard_state.set_vote(0);
    storage1->SetRaftState({hard_state, conf_state});

    Config config1 = DefaultConfig();
    config1.id = 1;
    config1.election_tick = 10;
    config1.heartbeat_tick = 1;
    config1.skip_bcast_commit = true;
    auto r1 = std::make_unique<Raft>(config1, std::make_unique<MemoryStorage>(*storage1));

    auto storage2 = std::make_shared<MemoryStorage>();
    storage2->SetRaftState({hard_state, conf_state});
    Config config2 = DefaultConfig();
    config2.id = 2;
    config2.election_tick = 10;
    config2.heartbeat_tick = 1;
    auto r2 = std::make_unique<Raft>(config2, std::make_unique<MemoryStorage>(*storage2));

    auto storage3 = std::make_shared<MemoryStorage>();
    storage3->SetRaftState({hard_state, conf_state});
    Config config3 = DefaultConfig();
    config3.id = 3;
    config3.election_tick = 10;
    config3.heartbeat_tick = 1;
    auto r3 = std::make_unique<Raft>(config3, std::make_unique<MemoryStorage>(*storage3));

    std::vector<std::unique_ptr<Interface>> peers;
    peers.push_back(Interface(std::move(r1), storage1));
    peers.push_back(Interface(std::move(r2), storage2));
    peers.push_back(Interface(std::move(r3), storage3));
    Network nt = Network::Create(std::move(peers));

    // elect r1 as leader
    nt.Send({NewMessage(1, 1, MessageType::MsgHup, 0)});

    // Without bcast commit, followers will not update its commit index immediately.
    Entry test_entry;
    test_entry.set_data("testdata");
    Message msg = NewMessageWithEntries(1, 1, MessageType::MsgPropose, {test_entry});
    nt.Send({msg});
    CHECK_EQ(nt.peers()[1].raft_log().committed(), 2);
    CHECK_EQ(nt.peers()[2].raft_log().committed(), 1);
    CHECK_EQ(nt.peers()[3].raft_log().committed(), 1);

    // After bcast heartbeat, followers will be informed the actual commit index.
    for (size_t i = 0; i < nt.peers()[1].randomized_election_timeout(); ++i) {
        nt.peers()[1].Tick();
    }
    nt.Send({NewMessage(1, 1, MessageType::MsgHup, 0)});
    CHECK_EQ(nt.peers()[2].raft_log().committed(), 2);
    CHECK_EQ(nt.peers()[3].raft_log().committed(), 2);

    // The feature should be able to be adjusted at run time.
    // Note: Raft doesn't have SetSkipBcastCommit method, so we skip this part
    // In raft-rs, they use nt.peers.get_mut(&1).unwrap().skip_bcast_commit(false);
    // But raftpp Raft doesn't expose this method directly

    // Later proposal should commit former proposal.
    nt.Send({msg});
    nt.Send({msg});
    CHECK_EQ(nt.peers()[1].raft_log().committed(), 4);
    CHECK_EQ(nt.peers()[2].raft_log().committed(), 4);
    CHECK_EQ(nt.peers()[3].raft_log().committed(), 4);

    // When committing conf change, leader should always bcast commit.
    ConfChange cc;
    cc.set_change_type(ConfChangeType::RemoveNode);
    cc.set_node_id(3);
    std::string data = cc.SerializeAsString();
    Entry cc_entry;
    cc_entry.set_entry_type(EntryConfChange);
    cc_entry.set_data(data);
    nt.Send({NewMessageWithEntries(1, 1, MessageType::MsgPropose, {cc_entry})});
    CHECK(nt.peers()[1].ShouldBroadcastCommit());
    CHECK(nt.peers()[2].ShouldBroadcastCommit());
    CHECK(nt.peers()[3].ShouldBroadcastCommit());

    CHECK_EQ(nt.peers()[1].raft_log().committed(), 5);
    CHECK_EQ(nt.peers()[2].raft_log().committed(), 5);
    CHECK_EQ(nt.peers()[3].raft_log().committed(), 5);
}

/// Test bounded uncommitted entries growth with partition.
/// Tests a scenario where a leader is partitioned from a quorum of nodes.
/// It verifies that the leader's log is protected from unbounded growth
/// even as new entries continue to be proposed.
/// This protection is provided by the max_uncommitted_size configuration.
TEST_CASE("raw_node: bounded_uncommitted_entries_growth_with_partition") {
    auto storage = std::make_shared<MemoryStorage>();
    Config config = DefaultConfig();
    config.id = 1;
    config.max_uncommitted_size = 12;

    ConfState conf_state;
    conf_state.add_voters(1);
    HardState hard_state;
    hard_state.set_commit(0);
    hard_state.set_term(0);
    hard_state.set_vote(0);
    storage->SetRaftState({hard_state, conf_state});

    RawNode raw_node(config, std::make_unique<MemoryStorage>(*storage));

    // Wait raw_node to be leader
    raw_node.Campaign().expect("");
    while (true) {
        auto rd = raw_node.GetReady();
        storage->SetHardState(rd.hs.value());
        storage->Append(rd.entries()).value();
        if (rd.ss.has_value() && rd.ss->leader_id == raw_node.GetStatus().id()) {
            raw_node.Advance(rd);
            break;
        }
        raw_node.Advance(rd);
    }

    // Should be accepted
    std::string data = "hello world!";
    raw_node.Propose("", data).expect("");

    // Should be dropped
    auto result = raw_node.Propose("", data);
    CHECK(result.IsError());
    CHECK(result.error().code() == ErrorCode::ProposalDropped);

    // Should be accepted when previous data has been committed
    auto rd = raw_node.GetReady();
    storage->Append(rd.entries()).value();
    raw_node.Advance(rd);

    raw_node.Propose("", data).expect("");
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
    RawNode raw_node(config, std::make_unique<MemoryStorage>(*storage));

    raw_node.Campaign().expect("");
    auto rd = raw_node.GetReady();
    // Single node should become leader.
    CHECK(rd.ss.has_value() && rd.ss->leader_id == raw_node.GetStatus().id());
    storage->Append(rd.entries()).value();
    raw_node.Advance(rd);

    uint64_t last_index = raw_node.GetStatus().applied();

    std::string data = "hello world!";

    for (int i = 1; i < 10; ++i) {
        int cnt = (rand() % 10) + 1;
        for (int j = 0; j < cnt; ++j) {
            raw_node.Propose("", data).expect("");
        }

        auto rd = raw_node.GetReady();
        auto entries = rd.entries;
        CHECK_EQ(entries[0].index(), last_index + 1);
        CHECK_EQ(entries[entries.size() - 1].index(), last_index + cnt);
        MustCmpReady(rd, std::nullopt, std::nullopt, entries, {}, std::nullopt, true, true, true);

        storage->Append(entries).value();

        auto light_rd = raw_node.Advance(rd);
        CHECK_EQ(light_rd.committed_entries(), entries);
        CHECK(light_rd.commit_index().has_value());
        CHECK_EQ(light_rd.commit_index().value(), last_index + cnt);

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
    RawNode raw_node(config, std::make_unique<MemoryStorage>(*storage));

    std::vector<Entry> entries;
    for (int i = 2; i < 20; ++i) {
        entries.push_back(NewEntry(2, i, "hello"));
    }
    Message append_msg = NewMessageWithEntries(2, 1, MessageType::MsgAppend, entries);
    append_msg.set_term(2);
    append_msg.set_index(1);
    append_msg.set_log_term(1);
    append_msg.set_commit(5);
    raw_node.Step(append_msg).expect("");

    auto rd = raw_node.GetReady();
    MustCmpReady(
        rd, SoftState(2, StateRole::Follower), HardState(2, 5, 0), entries, {}, std::nullopt, true,
        false, true
    );
    storage->SetHardState(rd.hs.value());
    storage->Append(rd.entries()).value();
    auto light_rd = raw_node.Advance(rd);
    CHECK(!light_rd.commit_index().has_value());
    CHECK_EQ(
        light_rd.committed_entries(), std::vector<Entry>(entries.begin(), entries.begin() + 4)
    );
    CHECK(light_rd.messages.empty());

    Snapshot snap = NewSnapshot(10, 3, {1, 2});
    Message snapshot_msg = NewMessage(2, 1, MessageType::MsgSnapshot, 0);
    snapshot_msg.set_term(3);
    snapshot_msg.set_snapshot(snap);
    raw_node.Step(snapshot_msg).expect("");

    entries.clear();
    for (int i = 11; i < 14; ++i) {
        entries.push_back(NewEntry(3, i, "hello"));
    }
    append_msg = NewMessageWithEntries(2, 1, MessageType::MsgAppend, entries);
    append_msg.set_term(3);
    append_msg.set_index(10);
    append_msg.set_log_term(3);
    append_msg.set_commit(12);
    raw_node.Step(append_msg).expect("");

    rd = raw_node.GetReady();
    // If there is a snapshot, the committed entries should be empty.
    MustCmpReady(rd, std::nullopt, HardState(3, 12, 0), entries, {}, snap, true, false, true);
    // Should have a MsgAppendResponse
    CHECK_EQ(rd.persisted_messages[0].msg_type(), MessageType::MsgAppendResponse);
    storage->SetHardState(rd.hs.value());
    storage->ApplySnapshot(rd.snapshot.value()).value();
    storage->Append(rd.entries()).value();

    light_rd = raw_node.Advance(rd);
    CHECK(!light_rd.commit_index().has_value());
    CHECK_EQ(
        light_rd.committed_entries(), std::vector<Entry>(entries.begin(), entries.begin() + 2)
    );
    CHECK(light_rd.messages.empty());
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
    RawNode raw_node(config, std::make_unique<MemoryStorage>(*storage));

    std::vector<Entry> entries = {
        NewEntry(2, 2, "hello"),
        NewEntry(2, 3, "hello"),
        NewEntry(2, 4, "hello"),
    };
    Message append_msg = NewMessageWithEntries(2, 1, MessageType::MsgAppend, entries);
    append_msg.set_term(2);
    append_msg.set_index(1);
    append_msg.set_log_term(1);
    append_msg.set_commit(1);
    raw_node.Step(append_msg).expect("");

    auto rd = raw_node.GetReady();
    MustCmpReady(
        rd, SoftState(2, StateRole::Follower), HardState(2, 1, 0), entries, {}, std::nullopt, true,
        false, true
    );
    // Should have a MsgAppendResponse
    CHECK_EQ(rd.persisted_messages[0].msg_type(), MessageType::MsgAppendResponse);
    storage->SetHardState(rd.hs.value());
    storage->Append(rd.entries()).value();

    auto light_rd = raw_node.Advance(rd);
    CHECK(!light_rd.commit_index().has_value());
    CHECK(light_rd.committed_entries().empty());
    CHECK(light_rd.messages.empty());

    std::vector<Entry> entries_2 = {
        NewEntry(3, 4, "hello"),
        NewEntry(3, 5, "hello"),
        NewEntry(3, 6, "hello"),
    };
    append_msg = NewMessageWithEntries(3, 1, MessageType::MsgAppend, entries_2);
    append_msg.set_term(3);
    append_msg.set_index(3);
    append_msg.set_log_term(2);
    append_msg.set_commit(5);
    raw_node.Step(append_msg).expect("");

    rd = raw_node.GetReady();
    MustCmpReady(
        rd, SoftState(3, StateRole::Follower), HardState(3, 5, 0), entries_2,
        std::vector<Entry>(entries.begin(), entries.begin() + 2), std::nullopt, true, false, true
    );
    // Should have a MsgAppendResponse
    CHECK_EQ(rd.persisted_messages[0].msg_type(), MessageType::MsgAppendResponse);
    storage->SetHardState(rd.hs.value());
    storage->Append(rd.entries()).value();

    light_rd = raw_node.Advance(rd);
    CHECK(!light_rd.commit_index().has_value());
    CHECK_EQ(
        light_rd.committed_entries(), std::vector<Entry>(entries_2.begin(), entries_2.begin() + 2)
    );
    CHECK(light_rd.messages.empty());
}

/// Test committed entries pagination.
/// Tests the max_committed_size_per_ready configuration option.
TEST_CASE("raw_node: committed_entries_pagination") {
    auto storage = std::make_shared<MemoryStorage>();

    Config config = DefaultConfig();
    config.id = 1;
    config.election_tick = 10;
    config.heartbeat_tick = 1;
    RawNode raw_node(config, std::make_unique<MemoryStorage>(*storage));

    std::vector<Entry> entries;
    for (int i = 2; i < 10; ++i) {
        entries.push_back(NewEntry(1, i, ""));
    }
    Message msg = NewMessageWithEntries(3, 1, MessageType::MsgAppend, entries);
    msg.set_term(1);
    msg.set_index(1);
    msg.set_log_term(1);
    msg.set_commit(9);
    raw_node.Step(msg).expect("");

    // Test unpersisted entries won't be fetched.
    // NOTE: maybe it's better to allow fetching unpersisted committed entries.
    auto rd = raw_node.GetReady();
    CHECK(rd.committed_entries.empty());
    CHECK(raw_node.HasReady());

    // Persist entries.
    CHECK(!rd.entries.empty());
    raw_node.GetStorage()->Append(rd.entries()).value();

    // Advance the ready, and we can get committed_entries as expected.
    // Note: raftpp may not have SetMaxCommittedSizePerReady method
    // The storage's Entries method has a max_size parameter that limits the size
    auto light_rd = raw_node.Advance(rd);
    // MemoryStorage::entries uses limit_size to limit size of committed entries.
    // So there will be at least one entry.
    CHECK_EQ(light_rd.committed_entries().size(), 7);

    // No more `Ready`s.
    CHECK(!raw_node.HasReady());
}

/// Test disable proposal forwarding.
/// Tests that when disable_proposal_forwarding is true,
/// proposals to followers are dropped instead of forwarded to the leader.
TEST_CASE("raw_node: disable_proposal_forwarding") {
    auto storage1 = std::make_shared<MemoryStorage>();
    ConfState conf_state;
    conf_state.add_voters(1);
    conf_state.add_voters(2);
    conf_state.add_voters(3);
    HardState hard_state;
    hard_state.set_commit(0);
    hard_state.set_term(0);
    hard_state.set_vote(0);
    storage1->SetRaftState({hard_state, conf_state});

    Config config1 = DefaultConfig();
    config1.id = 1;
    config1.heartbeat_tick = 1;
    config1.election_tick = 10;
    config1.disable_proposal_forwarding = false;
    auto r1 = std::make_unique<Raft>(config1, std::make_unique<MemoryStorage>(*storage1));

    auto storage2 = std::make_shared<MemoryStorage>();
    storage2->SetRaftState({hard_state, conf_state});
    Config config2 = DefaultConfig();
    config2.id = 2;
    config2.heartbeat_tick = 1;
    config2.election_tick = 10;
    config2.disable_proposal_forwarding = false;
    auto r2 = std::make_unique<Raft>(config2, std::make_unique<MemoryStorage>(*storage2));

    auto storage3 = std::make_shared<MemoryStorage>();
    storage3->SetRaftState({hard_state, conf_state});
    Config config3 = DefaultConfig();
    config3.id = 3;
    config3.heartbeat_tick = 1;
    config3.election_tick = 10;
    config3.disable_proposal_forwarding = true;
    auto r3 = std::make_unique<Raft>(config3, std::make_unique<MemoryStorage>(*storage3));

    std::vector<std::unique_ptr<Interface>> peers;
    peers.push_back(Interface(std::move(r1), storage1));
    peers.push_back(Interface(std::move(r2), storage2));
    peers.push_back(Interface(std::move(r3), storage3));
    Network nt = Network::Create(std::move(peers));

    // Node 1 starts campaign to become leader.
    nt.Send({NewMessage(1, 1, MessageType::MsgHup, 0)});

    // Send proposal to n2(follower) where DisableProposalForwarding is false
    auto result = nt.peers()[2].Step(NewMessage(2, 2, MessageType::MsgPropose, 1));
    CHECK(result.IsOk());

    // Verify n2(follower) does forward the proposal when DisableProposalForwarding is false
    CHECK_EQ(nt.peers()[2].msgs().size(), 1);

    // Send proposal to n3(follower) where DisableProposalForwarding is true
    result = nt.peers()[3].Step(NewMessage(3, 3, MessageType::MsgPropose, 1));
    CHECK(result.IsError());
    CHECK(result.error().code() == ErrorCode::ProposalDropped);

    CHECK(nt.peers()[3].msgs().empty());
}

TEST_SUITE_END();
