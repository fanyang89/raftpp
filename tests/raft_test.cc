// Port of raft-rs harness/tests/integration_cases/test_raft.rs
// Core Raft algorithm tests.

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "harness/interface.h"
#include "harness/test_util.h"
#include "raftpp.capnp.h"
#include "raftpp/core/capnp_util.h"
#include "raftpp/core/error.h"
#include "raftpp/core/progress.h"
#include "raftpp/core/progress_tracker.h"
#include "raftpp/core/raft_config.h"
#include "raftpp/core/raft_core.h"
#include "raftpp/core/types.h"

using namespace raftpp;

namespace {

// Helper to create a single-element vector with move semantics
template <typename T>
std::vector<T> MakeVec(T&& item) {
    std::vector<T> v;
    v.push_back(std::move(item));
    return v;
}

// Helper to create a two-element vector with move semantics (cloning first element)
template <typename T>
std::vector<T> MakeVec2Clone(const T& item) {
    std::vector<T> v;
    v.push_back(CloneMessage(item));
    v.push_back(CloneMessage(item));
    return v;
}

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

// Helper to create a vector with a single entry (using move semantics)
std::vector<Entry> MakeEntryVec(Entry&& entry) {
    std::vector<Entry> v;
    v.push_back(std::move(entry));
    return v;
}

}  // namespace

TEST_SUITE_BEGIN("raft");

TEST_CASE("raft: progress committed index") {
    auto network = CreateTestNetwork(3);

    // Set node 1 as Leader
    network.Send(MakeVec(NewMessage(1, 1, MessageType::MSG_HUP)));

    CHECK_EQ(network.GetPeer(1)->state(), StateRole::Leader);

    AssertRaftLog("#1: ", network.GetPeer(1)->raft_log(), 1, 0, 1);
    AssertRaftLog("#2: ", network.GetPeer(2)->raft_log(), 1, 0, 1);
    AssertRaftLog("#3: ", network.GetPeer(3)->raft_log(), 1, 0, 1);

    auto& prs1 = network.GetPeer(1)->progress_tracker();
    CHECK_EQ(prs1.progress_map().at(1).committed_index(), 1);
    CHECK_EQ(prs1.progress_map().at(2).committed_index(), 1);
    CHECK_EQ(prs1.progress_map().at(3).committed_index(), 1);

    // Test append entries between 1 and 2
    Message propose = NewMessageWithEntries(
        1, 1, MessageType::MSG_PROPOSE, MakeEntryVec(NewEntry(0, 0, "testdata"))
    );

    network.Cut(1, 3);
    network.Send(MakeVec2Clone(propose));
    network.Recover();

    AssertRaftLog("#1: ", network.GetPeer(1)->raft_log(), 3, 0, 3);
    AssertRaftLog("#2: ", network.GetPeer(2)->raft_log(), 3, 0, 3);
    AssertRaftLog("#3: ", network.GetPeer(3)->raft_log(), 1, 0, 1);

    CHECK_EQ(prs1.progress_map().at(1).committed_index(), 3);
    CHECK_EQ(prs1.progress_map().at(2).committed_index(), 3);
    CHECK_EQ(prs1.progress_map().at(3).committed_index(), 1);

    // Test heartbeat
    network.Send(MakeVec(NewMessage(1, 1, MessageType::MSG_BEAT)));

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

        network.Send(MakeVec(NewMessage(1, 1, MessageType::MSG_HUP)));

        CHECK_EQ(network.GetPeer(1)->state(), expected_state);
        CHECK_EQ(network.GetPeer(1)->term(), expected_term);
    }
}

TEST_CASE("raft: learner ignores hup") {
    auto storage = std::make_shared<MemoryStorage>();
    storage->SetRaftState(MakeRaftState(MakeHardState(1, 0, 0), MakeConfState({2, 3}, {1})));

    Config cfg = NewTestConfig(1, 5, 1);
    auto sm = NewTestRaftWithConfig(cfg, storage);

    Message hup = NewMessage(1, 1, MessageType::MSG_HUP);
    REQUIRE(sm.Step(hup).has_value());

    CHECK_EQ(sm->state(), StateRole::Follower);
    CHECK_EQ(sm->term(), 1);
    CHECK(sm.msgs().empty());
}

TEST_CASE("raft: invalid conf change v2 proposal dropped") {
    auto storage = std::make_shared<MemoryStorage>();
    auto sm = NewTestRaft(1, {1}, 5, 1, storage);

    Message hup = NewMessage(1, 1, MessageType::MSG_HUP);
    REQUIRE(sm.Step(hup).has_value());
    CHECK_EQ(sm->state(), StateRole::Leader);
    CHECK_EQ(sm->raft_log().LastIndex(), 1);

    Entry invalid = NewEntry(0, 0, "not-capnp");
    capnp_util::builder<msg::Entry>(invalid).setEntryType(EntryType::ENTRY_CONF_CHANGE_V2);
    Message propose =
        NewMessageWithEntries(1, 1, MessageType::MSG_PROPOSE, MakeEntryVec(std::move(invalid)));

    auto result = sm.Step(propose);
    CHECK_FALSE(result.has_value());
    CHECK(result.error() == RaftErrorCode::ProposalDropped);
    CHECK_EQ(sm->raft_log().LastIndex(), 1);
}

TEST_CASE("raft: log replication") {
    auto network = CreateTestNetwork(3);

    network.Send(MakeVec(NewMessage(1, 1, MessageType::MSG_HUP)));

    Message propose = NewMessageWithEntries(
        1, 1, MessageType::MSG_PROPOSE, MakeEntryVec(NewEntry(0, 0, "somedata"))
    );
    network.Send(MakeVec(std::move(propose)));

    // All nodes should have the same log
    for (size_t i = 1; i <= 3; ++i) {
        auto* peer = network.GetPeer(i);
        CHECK_EQ(peer->raft_log().committed(), 2);
    }
}

TEST_CASE("raft: single node commit") {
    auto network = CreateTestNetwork(1);

    network.Send(MakeVec(NewMessage(1, 1, MessageType::MSG_HUP)));

    Message propose = NewMessageWithEntries(
        1, 1, MessageType::MSG_PROPOSE, MakeEntryVec(NewEntry(0, 0, "somedata"))
    );
    network.Send(MakeVec(std::move(propose)));

    auto* peer = network.GetPeer(1);
    CHECK_EQ(peer->raft_log().committed(), 2);
}

TEST_CASE("raft: commit without majority") {
    auto network = CreateTestNetwork(5);

    network.Send(MakeVec(NewMessage(1, 1, MessageType::MSG_HUP)));

    // Isolate 3 nodes
    network.Cut(1, 3);
    network.Cut(1, 4);
    network.Cut(1, 5);

    Message propose = NewMessageWithEntries(
        1, 1, MessageType::MSG_PROPOSE, MakeEntryVec(NewEntry(0, 0, "somedata"))
    );
    network.Send(MakeVec(std::move(propose)));

    // Commit should not have advanced (only 2 nodes can communicate)
    auto* peer = network.GetPeer(1);
    CHECK_EQ(peer->raft_log().committed(), 1);
}

TEST_CASE("raft: commit with full partition recovery") {
    auto network = CreateTestNetwork(5);

    network.Send(MakeVec(NewMessage(1, 1, MessageType::MSG_HUP)));

    // Create partition
    network.Cut(1, 3);
    network.Cut(1, 4);
    network.Cut(1, 5);

    Message propose = NewMessageWithEntries(
        1, 1, MessageType::MSG_PROPOSE, MakeEntryVec(NewEntry(0, 0, "somedata"))
    );
    network.Send(MakeVec(std::move(propose)));

    // Recover
    network.Recover();

    // Send heartbeat to sync
    network.Send(MakeVec(NewMessage(1, 1, MessageType::MSG_BEAT)));

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
    network.Send(MakeVec(NewMessage(1, 1, MessageType::MSG_HUP)));

    // Node 3 tries to become candidate (will fail due to isolation)
    network.Send(MakeVec(NewMessage(3, 3, MessageType::MSG_HUP)));

    CHECK_EQ(network.GetPeer(1)->state(), StateRole::Leader);
    CHECK_EQ(network.GetPeer(3)->state(), StateRole::Candidate);

    // Recover and sync
    network.Recover();

    network.Send(MakeVec(NewMessage(1, 1, MessageType::MSG_BEAT)));

    // After recovery, node 3 should become follower
    // (it will receive heartbeat from leader with higher term)
    CHECK_EQ(network.GetPeer(3)->state(), StateRole::Follower);
}

TEST_CASE("raft: candidate concede") {
    auto network = CreateTestNetwork(3);

    // Isolate node 1
    network.Isolate(1);

    // Node 1 tries to become candidate
    network.Send(MakeVec(NewMessage(1, 1, MessageType::MSG_HUP)));

    CHECK_EQ(network.GetPeer(1)->state(), StateRole::Candidate);

    // Recover and let node 3 become leader
    network.Recover();

    network.Send(MakeVec(NewMessage(3, 3, MessageType::MSG_HUP)));

    // Node 3 should be leader (it can reach majority)
    CHECK_EQ(network.GetPeer(3)->state(), StateRole::Leader);

    // Node 1 should step down after receiving from new leader
    CHECK_EQ(network.GetPeer(1)->state(), StateRole::Follower);
}

TEST_CASE("raft: add node") {
    auto network = CreateTestNetwork(3);

    network.Send(MakeVec(NewMessage(1, 1, MessageType::MSG_HUP)));

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

    network.Send(MakeVec(NewMessage(1, 1, MessageType::MSG_HUP)));

    CHECK_EQ(network.GetPeer(1)->state(), StateRole::Leader);

    // Remove node 3 via conf change
    auto* leader = network.GetPeer(1);
    auto cc = MakeRemoveNodeCC(3);
    auto result = leader->ApplyConfChange(cc);
    CHECK(result);

    auto& prs = leader->progress_tracker();
    CHECK(prs.progress_map().find(3) == prs.progress_map().end());
}

// Tests group commit.
// 1. Logs should be replicated to at least different groups before committed;
// 2. all peers are configured to the same group, simple quorum should be used.
TEST_CASE("raft: group commit") {
    struct TestCase {
        std::vector<uint64_t> matches;
        std::vector<uint64_t> group_ids;
        uint64_t group_commit_index;   // expected commit with group commit enabled
        uint64_t quorum_commit_index;  // expected commit with group commit disabled
    };

    std::vector<TestCase> tests = {
        // Single
        {{1}, {0}, 1, 1},
        {{1}, {1}, 1, 1},
        // Odd
        {{2, 2, 1}, {1, 2, 1}, 2, 2},
        {{2, 2, 1}, {1, 1, 2}, 1, 2},
        {{2, 2, 1}, {1, 0, 1}, 1, 2},
        {{2, 2, 1}, {0, 0, 0}, 1, 2},
        // Even
        {{4, 2, 1, 3}, {0, 0, 0, 0}, 1, 2},
        {{4, 2, 1, 3}, {1, 0, 0, 0}, 1, 2},
        {{4, 2, 1, 3}, {0, 1, 0, 2}, 2, 2},
        {{4, 2, 1, 3}, {0, 2, 1, 0}, 1, 2},
        {{4, 2, 1, 3}, {1, 1, 1, 1}, 2, 2},
        {{4, 2, 1, 3}, {1, 1, 2, 1}, 1, 2},
        {{4, 2, 1, 3}, {1, 2, 1, 1}, 2, 2},
        {{4, 2, 1, 3}, {4, 3, 2, 1}, 2, 2},
    };

    for (size_t i = 0; i < tests.size(); i++) {
        const auto& tc = tests[i];

        // Create storage with initial conf state
        auto storage = std::make_shared<MemoryStorage>();

        // Add log entries
        uint64_t min_index = *std::min_element(tc.matches.begin(), tc.matches.end());
        uint64_t max_index = *std::max_element(tc.matches.begin(), tc.matches.end());
        std::vector<Entry> logs;
        for (uint64_t idx = min_index; idx <= max_index; idx++) {
            logs.push_back(EmptyEntry(idx, 1));
        }
        std::ignore = storage->Append(logs);

        // Set hard state and conf state
        HardState hs = MakeHardState(1, 0, 0);
        storage->SetRaftState(MakeRaftState(hs, MakeConfState({1})));

        // Create raft instance
        Config cfg = NewTestConfig(1, 5, 1);
        auto sm = NewTestRaftWithConfig(cfg, storage);

        // Add peers and set up progress
        std::vector<std::pair<uint64_t, uint64_t>> groups;
        for (size_t j = 0; j < tc.matches.size(); j++) {
            uint64_t id = j + 1;
            uint64_t m = tc.matches[j];
            uint64_t g = tc.group_ids[j];

            if (sm->progress_tracker().get(id) == nullptr) {
                auto cc = MakeAddNodeCC(id);
                std::ignore = sm->ApplyConfChange(cc);
            }
            auto* pr = sm->progress_tracker().get(id);
            pr->matched() = m;
            pr->next_idx() = m + 1;

            if (g != 0) {
                groups.emplace_back(id, g);
            }
        }

        // Enable group commit and assign groups (as follower, should not commit)
        sm->EnableGroupCommit(true);
        sm->AssignCommitGroups(groups);
        CHECK_MESSAGE(
            sm->raft_log().committed() == 0, "test #", i, ": follower group committed ",
            sm->raft_log().committed(), ", want 0"
        );

        // Set state to leader directly (like raft-rs does: sm.state = StateRole::Leader)
        // This avoids term changes that would prevent MaybeCommit from working
        sm.SetState(StateRole::Leader);
        sm->AssignCommitGroups(groups);
        CHECK_MESSAGE(
            sm->raft_log().committed() == tc.group_commit_index, "test #", i,
            ": leader group committed ", sm->raft_log().committed(), ", want ",
            tc.group_commit_index
        );

        // Disable group commit - should use simple quorum
        sm->EnableGroupCommit(false);
        CHECK_MESSAGE(
            sm->raft_log().committed() == tc.quorum_commit_index, "test #", i,
            ": quorum committed ", sm->raft_log().committed(), ", want ", tc.quorum_commit_index
        );
    }
}

TEST_CASE("raft: group commit consistent") {
    // Create logs: entries 1-5 at term 1, entries 6-8 at term 2
    std::vector<Entry> logs;
    for (uint64_t i = 1; i <= 5; i++) {
        logs.push_back(EmptyEntry(i, 1));
    }
    for (uint64_t i = 6; i <= 8; i++) {
        logs.push_back(EmptyEntry(i, 2));
    }

    struct TestCase {
        std::vector<uint64_t> matches;
        std::vector<uint64_t> group_ids;
        uint64_t committed;
        uint64_t applied;
        StateRole state;
        std::optional<bool> expected;
    };

    std::vector<TestCase> tests = {
        // Single node is not using group commit
        {{8}, {0}, 8, 6, StateRole::Leader, false},
        {{8}, {1}, 8, 5, StateRole::Leader, std::nullopt},
        {{8}, {1}, 8, 6, StateRole::Follower, std::nullopt},
        // Not commit to current term should return None
        {{8, 2, 0}, {1, 2, 1}, 2, 2, StateRole::Leader, std::nullopt},
        {{8, 2, 6}, {1, 1, 2}, 6, 6, StateRole::Leader, true},
        // Not apply to current term should return None
        {{8, 2, 6}, {1, 1, 2}, 6, 5, StateRole::Leader, std::nullopt},
        // It should be false when not using group commit
        {{8, 6, 6}, {0, 0, 0}, 6, 6, StateRole::Leader, false},
        // It should be false when there is only one group
        {{8, 6, 6}, {1, 1, 1}, 6, 6, StateRole::Leader, false},
        {{8, 6, 6}, {1, 1, 0}, 6, 6, StateRole::Leader, false},
        // Only leader knows what's the current state
        {{8, 2, 6}, {1, 1, 2}, 6, 6, StateRole::Follower, std::nullopt},
    };

    for (size_t i = 0; i < tests.size(); i++) {
        const auto& tc = tests[i];

        // Create storage with logs
        auto storage = std::make_shared<MemoryStorage>();
        std::ignore = storage->Append(logs);

        HardState hs = MakeHardState(2, 0, tc.committed);
        storage->SetRaftState(MakeRaftState(hs, MakeConfState({1})));

        Config cfg = NewTestConfig(1, 5, 1);
        cfg.applied = tc.applied;
        auto sm = NewTestRaftWithConfig(cfg, storage);

        // Add peers and set up progress
        std::vector<std::pair<uint64_t, uint64_t>> groups;
        for (size_t j = 0; j < tc.matches.size(); j++) {
            uint64_t id = j + 1;
            uint64_t m = tc.matches[j];
            uint64_t g = tc.group_ids[j];

            if (sm->progress_tracker().get(id) == nullptr) {
                auto cc = MakeAddNodeCC(id);
                std::ignore = sm->ApplyConfChange(cc);
            }
            auto* pr = sm->progress_tracker().get(id);
            pr->matched() = m;
            pr->next_idx() = m + 1;

            if (g != 0) {
                groups.emplace_back(id, g);
            }
        }

        sm->EnableGroupCommit(true);
        sm->AssignCommitGroups(groups);

        // Set state directly (like raft-rs does: sm.state = role)
        sm.SetState(tc.state);

        auto result = sm->CheckGroupCommitConsistent();
        CHECK_MESSAGE(
            result == tc.expected, "test #", i, ": got ",
            (result.has_value() ? (result.value() ? "true" : "false") : "nullopt"), ", want ",
            (tc.expected.has_value() ? (tc.expected.value() ? "true" : "false") : "nullopt")
        );
    }
}

TEST_SUITE_END();
