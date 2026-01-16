// Port of raft-rs harness/tests/integration_cases/test_raft.rs
// Core Raft algorithm tests.

#include <algorithm>
#include <map>
#include <vector>

#include <doctest/doctest.h>

#include "harness/network.h"
#include "harness/test_util.h"

using namespace raftpp;

namespace {

void AssertRaftLog(
    const std::string& prefix, const RaftLog& raft_log, uint64_t committed, uint64_t applied,
    uint64_t last
) {
    CHECK_MESSAGE(
        raft_log.committed() == committed, prefix, "committed = ", raft_log.committed(),
        ", want = ", committed
    );
    CHECK_MESSAGE(
        raft_log.applied() == applied, prefix, "applied = ", raft_log.applied(),
        ", want = ", applied
    );
    CHECK_MESSAGE(
        raft_log.LastIndex() == last, prefix, "last_index = ", raft_log.LastIndex(),
        ", want = ", last
    );
}

}  // namespace

TEST_SUITE_BEGIN("raft");

TEST_CASE("raft: progress committed index") {
    auto network = CreateTestNetwork(3);

    // Set node 1 as Leader
    Message hup;
    hup.set_msg_type(MsgHup);
    hup.set_from(1);
    hup.set_to(1);
    network.Send({hup});

    CHECK_EQ(network.GetPeer(1)->state(), StateRole::Leader);

    AssertRaftLog("#1: ", network.GetPeer(1)->raft_log(), 1, 0, 1);
    AssertRaftLog("#2: ", network.GetPeer(2)->raft_log(), 1, 0, 1);
    AssertRaftLog("#3: ", network.GetPeer(3)->raft_log(), 1, 0, 1);

    auto& prs1 = network.GetPeer(1)->progress_tracker();
    CHECK_EQ(prs1.progress_map().at(1).committed_index(), 1);
    CHECK_EQ(prs1.progress_map().at(2).committed_index(), 1);
    CHECK_EQ(prs1.progress_map().at(3).committed_index(), 1);

    // Test append entries between 1 and 2
    Entry test_entry;
    test_entry.set_data("testdata");

    Message propose;
    propose.set_msg_type(MsgPropose);
    propose.set_from(1);
    propose.set_to(1);
    *propose.add_entries() = test_entry;

    network.Cut(1, 3);
    network.Send({propose, propose});
    network.Recover();

    AssertRaftLog("#1: ", network.GetPeer(1)->raft_log(), 3, 0, 3);
    AssertRaftLog("#2: ", network.GetPeer(2)->raft_log(), 3, 0, 3);
    AssertRaftLog("#3: ", network.GetPeer(3)->raft_log(), 1, 0, 1);

    CHECK_EQ(prs1.progress_map().at(1).committed_index(), 3);
    CHECK_EQ(prs1.progress_map().at(2).committed_index(), 3);
    CHECK_EQ(prs1.progress_map().at(3).committed_index(), 1);

    // Test heartbeat
    Message heartbeat;
    heartbeat.set_msg_type(MsgBeat);
    heartbeat.set_from(1);
    heartbeat.set_to(1);
    network.Send({heartbeat});

    AssertRaftLog("#1: ", network.GetPeer(1)->raft_log(), 3, 0, 3);
    AssertRaftLog("#2: ", network.GetPeer(2)->raft_log(), 3, 0, 3);
    AssertRaftLog("#3: ", network.GetPeer(3)->raft_log(), 3, 0, 3);

    CHECK_EQ(prs1.progress_map().at(1).committed_index(), 3);
    CHECK_EQ(prs1.progress_map().at(2).committed_index(), 3);
    CHECK_EQ(prs1.progress_map().at(3).committed_index(), 3);
}

TEST_CASE("raft: leader election") {
    struct TestCase {
        size_t size;
        StateRole expected_state;
        uint64_t expected_term;
    };

    std::vector<TestCase> tests = {
        {1, StateRole::Leader, 1},
        {3, StateRole::Leader, 1},
        {5, StateRole::Leader, 1},
    };

    for (const auto& [size, expected_state, expected_term] : tests) {
        auto network = CreateTestNetwork(size);

        Message hup;
        hup.set_msg_type(MsgHup);
        hup.set_from(1);
        hup.set_to(1);
        network.Send({hup});

        CHECK_EQ(network.GetPeer(1)->state(), expected_state);
        CHECK_EQ(network.GetPeer(1)->term(), expected_term);
    }
}

TEST_CASE("raft: log replication") {
    auto network = CreateTestNetwork(3);

    Message hup;
    hup.set_msg_type(MsgHup);
    hup.set_from(1);
    hup.set_to(1);
    network.Send({hup});

    Message propose;
    propose.set_msg_type(MsgPropose);
    propose.set_from(1);
    propose.set_to(1);
    auto* e = propose.add_entries();
    e->set_data("somedata");
    network.Send({propose});

    // All nodes should have the same log
    for (size_t i = 1; i <= 3; ++i) {
        auto* peer = network.GetPeer(i);
        CHECK_EQ(peer->raft_log().committed(), 2);
    }
}

TEST_CASE("raft: single node commit") {
    auto network = CreateTestNetwork(1);

    Message hup;
    hup.set_msg_type(MsgHup);
    hup.set_from(1);
    hup.set_to(1);
    network.Send({hup});

    Message propose;
    propose.set_msg_type(MsgPropose);
    propose.set_from(1);
    propose.set_to(1);
    auto* e = propose.add_entries();
    e->set_data("somedata");
    network.Send({propose});

    auto* peer = network.GetPeer(1);
    CHECK_EQ(peer->raft_log().committed(), 2);
}

TEST_CASE("raft: commit without majority") {
    auto network = CreateTestNetwork(5);

    Message hup;
    hup.set_msg_type(MsgHup);
    hup.set_from(1);
    hup.set_to(1);
    network.Send({hup});

    // Isolate 3 nodes
    network.Cut(1, 3);
    network.Cut(1, 4);
    network.Cut(1, 5);

    Message propose;
    propose.set_msg_type(MsgPropose);
    propose.set_from(1);
    propose.set_to(1);
    auto* e = propose.add_entries();
    e->set_data("somedata");
    network.Send({propose});

    // Commit should not have advanced (only 2 nodes can communicate)
    auto* peer = network.GetPeer(1);
    CHECK_EQ(peer->raft_log().committed(), 1);
}

TEST_CASE("raft: commit with full partition recovery") {
    auto network = CreateTestNetwork(5);

    Message hup;
    hup.set_msg_type(MsgHup);
    hup.set_from(1);
    hup.set_to(1);
    network.Send({hup});

    // Create partition
    network.Cut(1, 3);
    network.Cut(1, 4);
    network.Cut(1, 5);

    Message propose;
    propose.set_msg_type(MsgPropose);
    propose.set_from(1);
    propose.set_to(1);
    auto* e = propose.add_entries();
    e->set_data("somedata");
    network.Send({propose});

    // Recover
    network.Recover();

    // Send heartbeat to sync
    Message beat;
    beat.set_msg_type(MsgBeat);
    beat.set_from(1);
    beat.set_to(1);
    network.Send({beat});

    // Now all nodes should be synced
    for (size_t i = 1; i <= 5; ++i) {
        auto* peer = network.GetPeer(i);
        CHECK_EQ(peer->raft_log().committed(), 2);
    }
}

TEST_CASE("raft: dueling candidates") {
    auto network = CreateTestNetwork(3);

    // Isolate node 3
    network.Isolate(3);

    // Node 1 becomes leader
    Message hup1;
    hup1.set_msg_type(MsgHup);
    hup1.set_from(1);
    hup1.set_to(1);
    network.Send({hup1});

    // Node 3 tries to become candidate (will fail due to isolation)
    Message hup3;
    hup3.set_msg_type(MsgHup);
    hup3.set_from(3);
    hup3.set_to(3);
    network.Send({hup3});

    CHECK_EQ(network.GetPeer(1)->state(), StateRole::Leader);
    CHECK_EQ(network.GetPeer(3)->state(), StateRole::Candidate);

    // Recover and sync
    network.Recover();

    Message beat;
    beat.set_msg_type(MsgBeat);
    beat.set_from(1);
    beat.set_to(1);
    network.Send({beat});

    // After recovery, node 3 should become follower
    // (it will receive heartbeat from leader with higher term)
    CHECK_EQ(network.GetPeer(3)->state(), StateRole::Follower);
}

TEST_CASE("raft: candidate concede") {
    auto network = CreateTestNetwork(3);

    // Isolate node 1
    network.Isolate(1);

    // Node 1 tries to become candidate
    Message hup1;
    hup1.set_msg_type(MsgHup);
    hup1.set_from(1);
    hup1.set_to(1);
    network.Send({hup1});

    CHECK_EQ(network.GetPeer(1)->state(), StateRole::Candidate);

    // Recover and let node 3 become leader
    network.Recover();

    Message hup3;
    hup3.set_msg_type(MsgHup);
    hup3.set_from(3);
    hup3.set_to(3);
    network.Send({hup3});

    // Node 3 should be leader (it can reach majority)
    CHECK_EQ(network.GetPeer(3)->state(), StateRole::Leader);

    // Node 1 should step down after receiving from new leader
    CHECK_EQ(network.GetPeer(1)->state(), StateRole::Follower);
}

TEST_CASE("raft: add node") {
    auto network = CreateTestNetwork(3);

    Message hup;
    hup.set_msg_type(MsgHup);
    hup.set_from(1);
    hup.set_to(1);
    network.Send({hup});

    CHECK_EQ(network.GetPeer(1)->state(), StateRole::Leader);

    // Add node 4 via conf change
    auto* leader = network.GetPeer(1);
    auto cc = MakeAddNodeCC(4);
    auto result = leader->ApplyConfChange(cc);
    CHECK(result);

    auto& prs = leader->progress_tracker();
    CHECK(prs.progress_map().find(4) != prs.progress_map().end());
}

TEST_CASE("raft: remove node") {
    auto network = CreateTestNetwork(3);

    Message hup;
    hup.set_msg_type(MsgHup);
    hup.set_from(1);
    hup.set_to(1);
    network.Send({hup});

    CHECK_EQ(network.GetPeer(1)->state(), StateRole::Leader);

    // Remove node 3 via conf change
    auto* leader = network.GetPeer(1);
    auto cc = MakeRemoveNodeCC(3);
    auto result = leader->ApplyConfChange(cc);
    CHECK(result);

    auto& prs = leader->progress_tracker();
    CHECK(prs.progress_map().find(3) == prs.progress_map().end());
}

TEST_SUITE_END();
