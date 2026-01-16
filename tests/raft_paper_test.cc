// Port of raft-rs harness/tests/integration_cases/test_raft_paper.rs
// Tests that verify the implementation against the Raft paper.

#include <doctest/doctest.h>

#include <algorithm>
#include <map>
#include <vector>

#include "harness/network.h"
#include "harness/test_util.h"

using namespace raftpp;

namespace {

constexpr const char* SOME_DATA = "somedata";

// Accept an append message and create a response.
Message AcceptAndReply(const Message& m) {
    CHECK_EQ(m.msg_type(), MsgAppend);
    Message reply;
    reply.set_msg_type(MsgAppendResponse);
    reply.set_from(m.to());
    reply.set_to(m.from());
    reply.set_term(m.term());
    reply.set_index(m.index() + m.entries_size());
    return reply;
}

// Commit the no-op entry that the leader creates after election.
void CommitNoopEntry(Interface& r, MemoryStorage& s) {
    CHECK_EQ(r->state(), StateRole::Leader);
    r->BroadcastAppend();

    // Simulate the response of MsgAppend
    auto msgs = r.ReadMessages();
    for (const auto& m : msgs) {
        CHECK_EQ(m.msg_type(), MsgAppend);
        CHECK_EQ(m.entries_size(), 1);
        CHECK(m.entries(0).data().empty());
        auto reply = AcceptAndReply(m);
        r.Step(reply);
    }

    // Ignore further messages to refresh followers' commit index
    r.ReadMessages();
    r.Persist();

    uint64_t committed = r->raft_log().committed();
    r->CommitApply(committed);
}

// Test that if one server's current term is smaller than the other's,
// then it updates its current term to the larger value.
// Reference: section 5.1
void TestUpdateTermFromMessage(StateRole state) {
    auto storage = std::make_shared<MemoryStorage>();
    auto r = NewTestRaft(1, {1, 2, 3}, 10, 1, storage);

    switch (state) {
        case StateRole::Follower:
            r->BecomeFollower(1, 2);
            break;
        case StateRole::PreCandidate:
            r->BecomePreCandidate();
            break;
        case StateRole::Candidate:
            r->BecomeCandidate();
            break;
        case StateRole::Leader:
            r->BecomeCandidate();
            r->BecomeLeader();
            break;
    }

    Message m;
    m.set_msg_type(MsgAppend);
    m.set_term(2);
    r.Step(m);

    CHECK_EQ(r->term(), 2);
    CHECK_EQ(r->state(), StateRole::Follower);
}

// Test that if a follower/candidate receives no communication over election timeout,
// it begins an election.
// Reference: section 5.2
void TestNonleaderStartElection(StateRole state) {
    size_t et = 10;  // election timeout
    auto storage = std::make_shared<MemoryStorage>();
    auto r = NewTestRaft(1, {1, 2, 3}, et, 1, storage);

    switch (state) {
        case StateRole::Follower:
            r->BecomeFollower(1, 2);
            break;
        case StateRole::Candidate:
            r->BecomeCandidate();
            break;
        default:
            FAIL("Only non-leader role is accepted.");
    }

    for (size_t i = 1; i < 2 * et; ++i) {
        r->Tick();
    }

    CHECK_EQ(r->term(), 2);
    CHECK_EQ(r->state(), StateRole::Candidate);
    CHECK(r->progress_tracker().votes().at(r->id()));

    auto msgs = r.ReadMessages();
    std::sort(msgs.begin(), msgs.end(), [](const Message& a, const Message& b) {
        return a.to() < b.to();
    });

    CHECK_EQ(msgs.size(), 2);
    for (size_t i = 0; i < msgs.size(); ++i) {
        CHECK_EQ(msgs[i].msg_type(), MsgRequestVote);
        CHECK_EQ(msgs[i].to(), i + 2);
        CHECK_EQ(msgs[i].term(), 2);
    }
}

// Test that election timeout for follower or candidate is randomized.
// Reference: section 5.2
void TestNonLeaderElectionTimeoutRandomized(StateRole state) {
    size_t et = 10;
    auto storage = std::make_shared<MemoryStorage>();
    auto r = NewTestRaft(1, {1, 2, 3}, et, 1, storage);

    std::map<size_t, bool> timeouts;
    for (size_t round = 0; round < 1000 * et; ++round) {
        uint64_t term = r->term();
        switch (state) {
            case StateRole::Follower:
                r->BecomeFollower(term + 1, 2);
                break;
            case StateRole::Candidate:
                r->BecomeCandidate();
                break;
            default:
                FAIL("Only non-leader state is accepted!");
        }

        size_t time = 0;
        while (r.ReadMessages().empty()) {
            r->Tick();
            ++time;
        }
        timeouts[time] = true;
    }

    CHECK(timeouts.size() <= et);
    CHECK(timeouts.size() >= et - 1);
    for (size_t d = et + 1; d < 2 * et; ++d) {
        CHECK(timeouts.count(d) > 0);
    }
}

// Test that in most cases only a single server will time out.
// Reference: section 5.2
void TestNonleadersElectionTimeoutNonconflict(StateRole state) {
    size_t et = 10;
    size_t size = 5;

    std::vector<Interface> rs;
    std::vector<uint64_t> ids;
    for (size_t i = 1; i <= size; ++i) {
        ids.push_back(i);
    }

    for (size_t i = 1; i <= size; ++i) {
        auto storage = std::make_shared<MemoryStorage>();
        rs.push_back(NewTestRaft(i, ids, et, 1, storage));
    }

    int conflicts = 0;
    for (size_t round = 0; round < 1000; ++round) {
        for (auto& r : rs) {
            uint64_t term = r->term();
            switch (state) {
                case StateRole::Follower:
                    r->BecomeFollower(term + 1, INVALID_ID);
                    break;
                case StateRole::Candidate:
                    r->BecomeCandidate();
                    break;
                default:
                    FAIL("Non-leader state is expected!");
            }
        }

        int timeout_num = 0;
        while (timeout_num == 0) {
            for (auto& r : rs) {
                r->Tick();
                if (!r.ReadMessages().empty()) {
                    ++timeout_num;
                }
            }
        }

        if (timeout_num > 1) {
            ++conflicts;
        }
    }

    CHECK(static_cast<double>(conflicts) / 1000.0 <= 0.3);
}

}  // namespace

TEST_SUITE_BEGIN("raft_paper");

// Section 5.1 - Terms

TEST_CASE("raft paper: follower update term from message") {
    TestUpdateTermFromMessage(StateRole::Follower);
}

TEST_CASE("raft paper: candidate update term from message") {
    TestUpdateTermFromMessage(StateRole::Candidate);
}

TEST_CASE("raft paper: leader update term from message") {
    TestUpdateTermFromMessage(StateRole::Leader);
}

// Section 5.2 - Leader Election

TEST_CASE("raft paper: start as follower") {
    auto storage = std::make_shared<MemoryStorage>();
    auto r = NewTestRaft(1, {1, 2, 3}, 10, 1, storage);
    CHECK_EQ(r->state(), StateRole::Follower);
}

TEST_CASE("raft paper: leader bcast beat") {
    size_t hi = 1;  // heartbeat interval
    auto storage = std::make_shared<MemoryStorage>();
    auto r = NewTestRaft(1, {1, 2, 3}, 10, hi, storage);
    r->BecomeCandidate();
    r->BecomeLeader();

    for (size_t i = 0; i < 10; ++i) {
        std::vector<Entry> entries = {EmptyEntry(0, i + 1)};
        r->AppendEntry(entries);
    }

    for (size_t i = 0; i < hi; ++i) {
        r->Tick();
    }

    auto msgs = r.ReadMessages();
    std::sort(msgs.begin(), msgs.end(), [](const Message& a, const Message& b) {
        return a.to() < b.to();
    });

    CHECK_EQ(msgs.size(), 2);
    for (const auto& m : msgs) {
        CHECK_EQ(m.msg_type(), MsgHeartbeat);
        CHECK_EQ(m.term(), 1);
    }
}

TEST_CASE("raft paper: follower start election") {
    TestNonleaderStartElection(StateRole::Follower);
}

TEST_CASE("raft paper: candidate start new election") {
    TestNonleaderStartElection(StateRole::Candidate);
}

TEST_CASE("raft paper: leader election in one round rpc") {
    struct TestCase {
        size_t size;
        std::map<uint64_t, bool> votes;
        StateRole state;
    };

    std::vector<TestCase> tests = {
        // Win the election when receiving votes from a majority
        {1, {}, StateRole::Leader},
        {3, {{2, true}, {3, true}}, StateRole::Leader},
        {3, {{2, true}}, StateRole::Leader},
        {5, {{2, true}, {3, true}, {4, true}, {5, true}}, StateRole::Leader},
        {5, {{2, true}, {3, true}, {4, true}}, StateRole::Leader},
        {5, {{2, true}, {3, true}}, StateRole::Leader},
        // Return to follower state if it receives vote denial from a majority
        {3, {{2, false}, {3, false}}, StateRole::Follower},
        {5, {{2, false}, {3, false}, {4, false}, {5, false}}, StateRole::Follower},
        {5, {{2, true}, {3, false}, {4, false}, {5, false}}, StateRole::Follower},
        // Stay in candidate if it does not obtain the majority
        {3, {}, StateRole::Candidate},
        {5, {{2, true}}, StateRole::Candidate},
        {5, {{2, false}, {3, false}}, StateRole::Candidate},
        {5, {}, StateRole::Candidate},
    };

    for (size_t i = 0; i < tests.size(); ++i) {
        const auto& [size, votes, expected_state] = tests[i];

        std::vector<uint64_t> peers;
        for (size_t j = 1; j <= size; ++j) {
            peers.push_back(j);
        }

        auto storage = std::make_shared<MemoryStorage>();
        auto r = NewTestRaft(1, peers, 10, 1, storage);

        Message hup;
        hup.set_msg_type(MsgHup);
        hup.set_from(1);
        hup.set_to(1);
        r.Step(hup);

        for (const auto& [id, vote] : votes) {
            Message m;
            m.set_msg_type(MsgRequestVoteResponse);
            m.set_from(id);
            m.set_to(1);
            m.set_term(r->term());
            m.set_reject(!vote);
            r.Step(m);
        }

        CHECK_EQ(r->state(), expected_state);
        CHECK_EQ(r->term(), 1);
    }
}

TEST_CASE("raft paper: follower vote") {
    struct TestCase {
        uint64_t vote;
        uint64_t nvote;
        bool wreject;
    };

    std::vector<TestCase> tests = {
        {INVALID_ID, 1, false},
        {INVALID_ID, 2, false},
        {1, 1, false},
        {2, 2, false},
        {1, 2, true},
        {2, 1, true},
    };

    for (size_t i = 0; i < tests.size(); ++i) {
        const auto& [vote, nvote, wreject] = tests[i];

        auto storage = std::make_shared<MemoryStorage>();
        auto r = NewTestRaft(1, {1, 2, 3}, 10, 1, storage);
        r->LoadState(MakeHardState(1, 0, vote));

        Message m;
        m.set_msg_type(MsgRequestVote);
        m.set_from(nvote);
        m.set_to(1);
        m.set_term(1);
        r.Step(m);

        auto msgs = r.ReadMessages();
        CHECK_EQ(msgs.size(), 1);
        CHECK_EQ(msgs[0].msg_type(), MsgRequestVoteResponse);
        CHECK_EQ(msgs[0].reject(), wreject);
    }
}

TEST_CASE("raft paper: candidate fallback") {
    auto storage1 = std::make_shared<MemoryStorage>();
    auto r1 = NewTestRaft(1, {1, 2, 3}, 10, 1, storage1);

    Message hup;
    hup.set_msg_type(MsgHup);
    hup.set_from(1);
    hup.set_to(1);
    r1.Step(hup);
    CHECK_EQ(r1->state(), StateRole::Candidate);

    Message m1;
    m1.set_msg_type(MsgAppend);
    m1.set_from(2);
    m1.set_to(1);
    m1.set_term(2);
    r1.Step(m1);
    CHECK_EQ(r1->state(), StateRole::Follower);
    CHECK_EQ(r1->term(), 2);

    auto storage2 = std::make_shared<MemoryStorage>();
    auto r2 = NewTestRaft(1, {1, 2, 3}, 10, 1, storage2);
    r2.Step(hup);
    CHECK_EQ(r2->state(), StateRole::Candidate);

    Message m2;
    m2.set_msg_type(MsgAppend);
    m2.set_from(2);
    m2.set_to(1);
    m2.set_term(3);
    r2.Step(m2);
    CHECK_EQ(r2->state(), StateRole::Follower);
    CHECK_EQ(r2->term(), 3);
}

TEST_CASE("raft paper: follower election timeout randomized") {
    TestNonLeaderElectionTimeoutRandomized(StateRole::Follower);
}

TEST_CASE("raft paper: candidate election timeout randomized") {
    TestNonLeaderElectionTimeoutRandomized(StateRole::Candidate);
}

TEST_CASE("raft paper: follower election timeout nonconflict") {
    TestNonleadersElectionTimeoutNonconflict(StateRole::Follower);
}

TEST_CASE("raft paper: candidates election timeout nonconflict") {
    TestNonleadersElectionTimeoutNonconflict(StateRole::Candidate);
}

// Section 5.3 - Log Replication

TEST_CASE("raft paper: leader start replication") {
    auto storage = std::make_shared<MemoryStorage>();
    auto r = NewTestRaft(1, {1, 2, 3}, 10, 1, storage);
    r->BecomeCandidate();
    r->BecomeLeader();
    CommitNoopEntry(r, *storage);

    uint64_t li = r->raft_log().LastIndex();

    Message propose;
    propose.set_msg_type(MsgPropose);
    propose.set_from(1);
    propose.set_to(1);
    auto* e = propose.add_entries();
    e->set_data(SOME_DATA);
    r.Step(propose);

    CHECK_EQ(r->raft_log().LastIndex(), li + 1);
    CHECK_EQ(r->raft_log().committed(), li);

    auto msgs = r.ReadMessages();
    std::sort(msgs.begin(), msgs.end(), [](const Message& a, const Message& b) {
        return a.to() < b.to();
    });

    CHECK_EQ(msgs.size(), 2);
    for (const auto& m : msgs) {
        CHECK_EQ(m.msg_type(), MsgAppend);
        CHECK_EQ(m.index(), li);
        CHECK_EQ(m.log_term(), 1);
        CHECK_EQ(m.entries_size(), 1);
        CHECK_EQ(m.entries(0).data(), SOME_DATA);
    }
}

TEST_CASE("raft paper: leader commit entry") {
    auto storage = std::make_shared<MemoryStorage>();
    auto r = NewTestRaft(1, {1, 2, 3}, 10, 1, storage);
    r->BecomeCandidate();
    r->BecomeLeader();
    CommitNoopEntry(r, *storage);

    uint64_t li = r->raft_log().LastIndex();

    Message propose;
    propose.set_msg_type(MsgPropose);
    propose.set_from(1);
    propose.set_to(1);
    auto* e = propose.add_entries();
    e->set_data(SOME_DATA);
    r.Step(propose);
    r.Persist();

    for (const auto& m : r.ReadMessages()) {
        auto reply = AcceptAndReply(m);
        r.Step(reply);
    }

    CHECK_EQ(r->raft_log().committed(), li + 1);
}

TEST_CASE("raft paper: leader acknowledge commit") {
    struct TestCase {
        size_t size;
        std::map<uint64_t, bool> acceptors;
        bool wack;
    };

    std::vector<TestCase> tests = {
        {1, {}, true},
        {3, {}, false},
        {3, {{2, true}}, true},
        {3, {{2, true}, {3, true}}, true},
        {5, {}, false},
        {5, {{2, true}}, false},
        {5, {{2, true}, {3, true}}, true},
        {5, {{2, true}, {3, true}, {4, true}}, true},
        {5, {{2, true}, {3, true}, {4, true}, {5, true}}, true},
    };

    for (size_t i = 0; i < tests.size(); ++i) {
        const auto& [size, acceptors, wack] = tests[i];

        std::vector<uint64_t> peers;
        for (size_t j = 1; j <= size; ++j) {
            peers.push_back(j);
        }

        auto storage = std::make_shared<MemoryStorage>();
        auto r = NewTestRaft(1, peers, 10, 1, storage);
        r->BecomeCandidate();
        r->BecomeLeader();
        CommitNoopEntry(r, *storage);

        uint64_t li = r->raft_log().LastIndex();

        Message propose;
        propose.set_msg_type(MsgPropose);
        propose.set_from(1);
        propose.set_to(1);
        auto* e = propose.add_entries();
        e->set_data(SOME_DATA);
        r.Step(propose);
        r.Persist();

        for (const auto& m : r.ReadMessages()) {
            if (acceptors.count(m.to()) && acceptors.at(m.to())) {
                auto reply = AcceptAndReply(m);
                r.Step(reply);
            }
        }

        bool g = r->raft_log().committed() > li;
        CHECK_EQ(g, wack);
    }
}

// Section 5.4 - Safety

TEST_CASE("raft paper: vote request") {
    struct TestCase {
        std::vector<Entry> ents;
        uint64_t wterm;
    };

    std::vector<TestCase> tests = {
        {{EmptyEntry(1, 1)}, 2},
        {{EmptyEntry(1, 1), EmptyEntry(2, 2)}, 3},
    };

    for (size_t j = 0; j < tests.size(); ++j) {
        const auto& [ents, wterm] = tests[j];

        auto storage = std::make_shared<MemoryStorage>();
        auto r = NewTestRaft(1, {1, 2, 3}, 10, 1, storage);

        Message m;
        m.set_msg_type(MsgAppend);
        m.set_from(2);
        m.set_to(1);
        m.set_term(wterm - 1);
        m.set_log_term(0);
        m.set_index(0);
        for (const auto& e : ents) {
            *m.add_entries() = e;
        }
        r.Step(m);
        r.ReadMessages();

        size_t election_timeout = 10;  // from config
        for (size_t i = 1; i < election_timeout * 2; ++i) {
            r->TickElection();
        }

        auto msgs = r.ReadMessages();
        std::sort(msgs.begin(), msgs.end(), [](const Message& a, const Message& b) {
            return a.to() < b.to();
        });

        CHECK_EQ(msgs.size(), 2);
        for (size_t i = 0; i < msgs.size(); ++i) {
            CHECK_EQ(msgs[i].msg_type(), MsgRequestVote);
            CHECK_EQ(msgs[i].to(), i + 2);
            CHECK_EQ(msgs[i].term(), wterm);
            CHECK_EQ(msgs[i].index(), ents.back().index());
            CHECK_EQ(msgs[i].log_term(), ents.back().term());
        }
    }
}

TEST_CASE("raft paper: voter") {
    struct TestCase {
        std::vector<Entry> ents;
        uint64_t log_term;
        uint64_t index;
        bool wreject;
    };

    std::vector<TestCase> tests = {
        // Same logterm
        {{EmptyEntry(1, 1)}, 1, 1, false},
        {{EmptyEntry(1, 1)}, 1, 2, false},
        {{EmptyEntry(1, 1), EmptyEntry(1, 2)}, 1, 1, true},
        // Candidate higher logterm
        {{EmptyEntry(1, 1)}, 2, 1, false},
        {{EmptyEntry(1, 1)}, 2, 2, false},
        {{EmptyEntry(1, 1), EmptyEntry(1, 2)}, 2, 1, false},
        // Voter higher logterm
        {{EmptyEntry(2, 1)}, 1, 1, true},
        {{EmptyEntry(2, 1)}, 1, 2, true},
        {{EmptyEntry(2, 1), EmptyEntry(1, 2)}, 1, 1, true},
    };

    for (size_t i = 0; i < tests.size(); ++i) {
        const auto& [ents, log_term, index, wreject] = tests[i];

        auto storage = std::make_shared<MemoryStorage>();
        // Set conf_state directly instead of using ApplySnapshot
        // (ApplySnapshot would fail because first_index > snap.index)
        ConfState conf_state;
        conf_state.add_voters(1);
        conf_state.add_voters(2);
        storage->SetConfState(conf_state);
        storage->Append(ents);

        auto r = NewTestRaftWithConfig(NewTestConfig(1, 10, 1), storage);

        Message m;
        m.set_msg_type(MsgRequestVote);
        m.set_from(2);
        m.set_to(1);
        m.set_term(3);
        m.set_log_term(log_term);
        m.set_index(index);
        r.Step(m);

        auto msgs = r.ReadMessages();
        CHECK_EQ(msgs.size(), 1);
        CHECK_EQ(msgs[0].msg_type(), MsgRequestVoteResponse);
        CHECK_EQ(msgs[0].reject(), wreject);
    }
}

TEST_CASE("raft paper: leader only commits log from current term") {
    std::vector<Entry> ents = {EmptyEntry(1, 1), EmptyEntry(2, 2)};

    struct TestCase {
        uint64_t index;
        uint64_t wcommit;
    };

    std::vector<TestCase> tests = {
        // Do not commit log entries in previous terms
        {1, 0},
        {2, 0},
        // Commit log in current term
        {3, 3},
    };

    for (size_t i = 0; i < tests.size(); ++i) {
        const auto& [index, wcommit] = tests[i];

        auto storage = std::make_shared<MemoryStorage>();
        // Set conf_state directly instead of using ApplySnapshot
        // (ApplySnapshot would fail because first_index > snap.index)
        ConfState conf_state;
        conf_state.add_voters(1);
        conf_state.add_voters(2);
        storage->SetConfState(conf_state);
        storage->Append(ents);

        auto r = NewTestRaftWithConfig(NewTestConfig(1, 10, 1), storage);
        r->LoadState(MakeHardState(2, 0, 0));

        // Become leader at term 3
        r->BecomeCandidate();
        r->BecomeLeader();
        r.ReadMessages();

        // Propose an entry to current term
        Message propose;
        propose.set_msg_type(MsgPropose);
        propose.set_from(1);
        propose.set_to(1);
        auto* e = propose.add_entries();
        e->set_data(SOME_DATA);
        r.Step(propose);
        r.Persist();

        Message resp;
        resp.set_msg_type(MsgAppendResponse);
        resp.set_from(2);
        resp.set_to(1);
        resp.set_term(r->term());
        resp.set_index(index);
        r.Step(resp);

        CHECK_EQ(r->raft_log().committed(), wcommit);
    }
}

TEST_SUITE_END();
