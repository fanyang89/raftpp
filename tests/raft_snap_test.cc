// Port of raft-rs harness/tests/integration_cases/test_raft_snap.rs
// Snapshot-related tests.

#include <doctest/doctest.h>

#include "harness/network.h"
#include "harness/test_util.h"

using namespace raftpp;

namespace {

Snapshot TestingSnapshot() {
    return NewSnapshot(11, 11, {1, 2});
}

}  // namespace

TEST_SUITE_BEGIN("raft_snap");

TEST_CASE("sending snapshot set pending snapshot") {
    auto storage = std::make_shared<MemoryStorage>();
    auto r = NewTestRaft(1, {1, 2}, 10, 1, storage);
    auto restore_result = r->Restore(NewSnapshot(11, 11, {1, 2}));
    CHECK(restore_result);
    r.Persist();

    r->BecomeCandidate();
    r->BecomeLeader();

    // force set the next of node 1, so that
    // node 1 needs a snapshot
    auto& pr = r->progress_tracker().progress_map().at(2);
    pr.next_idx() = r->raft_log().LastIndex();

    Message m;
    auto builder = m.builder();
    builder.setMsgType(MessageType::MSG_APPEND_RESPONSE);
    builder.setFrom(2);
    builder.setTo(1);
    auto& voter_2 = r->progress_tracker().progress_map().at(2);
    builder.setIndex(voter_2.next_idx() - 1);
    builder.setReject(true);

    auto result = r.Step(m);
    (void)result;

    CHECK_EQ(voter_2.pending_snapshot(), 11);
}

TEST_CASE("pending snapshot pause replication") {
    auto storage = std::make_shared<MemoryStorage>();
    auto r = NewTestRaft(1, {1, 2}, 10, 1, storage);
    auto restore_result = r->Restore(NewSnapshot(11, 11, {1, 2}));
    CHECK(restore_result);
    r.Persist();

    r->BecomeCandidate();
    r->BecomeLeader();

    auto& pr = r->progress_tracker().progress_map().at(2);
    pr.BecomeSnapshot(11);

    Entry entry = NewEntry(0, 0, "test");
    Message m = NewMessageWithEntries(1, 1, MessageType::MSG_PROPOSE, std::vector<Entry>{entry});
    auto result = r.Step(m);
    (void)result;

    auto msgs = r.ReadMessages();
    CHECK(msgs.empty());
}

TEST_CASE("snapshot failure") {
    auto storage = std::make_shared<MemoryStorage>();
    auto r = NewTestRaft(1, {1, 2}, 10, 1, storage);
    auto restore_result = r->Restore(NewSnapshot(11, 11, {1, 2}));
    CHECK(restore_result);
    r.Persist();

    r->BecomeCandidate();
    r->BecomeLeader();

    auto& pr = r->progress_tracker().progress_map().at(2);
    pr.next_idx() = 1;
    pr.BecomeSnapshot(11);

    Message m;
    auto builder = m.builder();
    builder.setMsgType(MessageType::MSG_SNAP_STATUS);
    builder.setFrom(2);
    builder.setTo(1);
    builder.setReject(true);
    auto result = r.Step(m);
    (void)result;

    auto& voter_2 = r->progress_tracker().progress_map().at(2);
    CHECK_EQ(voter_2.pending_snapshot(), 0);
    CHECK_EQ(voter_2.next_idx(), 1);
    CHECK(voter_2.IsPaused());
}

TEST_CASE("snapshot succeed") {
    auto storage = std::make_shared<MemoryStorage>();
    auto r = NewTestRaft(1, {1, 2}, 10, 1, storage);
    auto restore_result = r->Restore(NewSnapshot(11, 11, {1, 2}));
    CHECK(restore_result);
    r.Persist();

    r->BecomeCandidate();
    r->BecomeLeader();

    auto& pr = r->progress_tracker().progress_map().at(2);
    pr.next_idx() = 1;
    pr.BecomeSnapshot(11);

    Message m;
    auto builder = m.builder();
    builder.setMsgType(MessageType::MSG_SNAP_STATUS);
    builder.setFrom(2);
    builder.setTo(1);
    builder.setReject(false);
    auto result = r.Step(m);
    (void)result;

    auto& voter_2 = r->progress_tracker().progress_map().at(2);
    CHECK_EQ(voter_2.pending_snapshot(), 0);
    CHECK_EQ(voter_2.next_idx(), 12);
    CHECK(voter_2.IsPaused());
}

TEST_CASE("snapshot abort") {
    auto storage = std::make_shared<MemoryStorage>();
    auto r = NewTestRaft(1, {1, 2}, 10, 1, storage);
    auto restore_result = r->Restore(NewSnapshot(11, 11, {1, 2}));
    CHECK(restore_result);
    r.Persist();

    r->BecomeCandidate();
    r->BecomeLeader();

    auto& pr = r->progress_tracker().progress_map().at(2);
    pr.next_idx() = 1;
    pr.BecomeSnapshot(11);

    Message m;
    auto builder = m.builder();
    builder.setMsgType(MessageType::MSG_APPEND_RESPONSE);
    builder.setFrom(2);
    builder.setTo(1);
    builder.setIndex(11);
    // A successful MsgAppendResponse that has a higher/equal index than the
    // pending snapshot should abort the pending snapshot.
    auto result = r.Step(m);
    (void)result;

    CHECK_EQ(pr.pending_snapshot(), 0);
    CHECK_EQ(pr.next_idx(), 12);
}

// Initialized storage should be at term 1 instead of 0. Otherwise the case will fail.
TEST_CASE("snapshot with min term") {
    auto do_test = [](bool pre_vote) {
        auto s1 = std::make_shared<MemoryStorage>();
        auto snap = NewSnapshot(1, 1, {1, 2});
        s1->ApplySnapshot(snap).value();

        auto n1 = NewTestRaftWithPrevote(1, {1, 2}, 10, 1, s1, pre_vote);
        auto n2 = NewTestRaftWithPrevote(2, {}, 10, 1, std::make_shared<MemoryStorage>(), pre_vote);

        std::vector<std::unique_ptr<Interface>> peers;
        peers.push_back(std::make_unique<Interface>(std::move(n1)));
        peers.push_back(std::make_unique<Interface>(std::move(n2)));
        auto network = Network::Create(std::move(peers));

        Message hup = NewMessage(1, 1, MessageType::MSG_HUP);
        network.Send({hup});

        // 1 will be elected as leader, and then send a snapshot and an empty entry to 2.
        CHECK_EQ(network.GetPeer(2)->raft_log().LastIndex(), 2);
        CHECK_EQ(network.GetPeer(2)->raft_log().LastIndex(), 2);
    };

    do_test(true);
    do_test(false);
}

TEST_CASE("request snapshot") {
    auto storage = std::make_shared<MemoryStorage>();
    auto r = NewTestRaft(1, {1, 2}, 10, 1, storage);
    auto restore_result = r->Restore(NewSnapshot(11, 11, {1, 2}));
    CHECK(restore_result);
    r.Persist();

    // Raft can not step request snapshot if there is no leader.
    auto result = r->RequestSnapshot();
    CHECK(!result);

    uint64_t term = r->term();
    r->BecomeFollower(term + 1, 2);

    // Raft can not step request snapshot if last raft log's term mismatch current term.
    result = r->RequestSnapshot();
    CHECK(!result);

    r->BecomeCandidate();
    r->BecomeLeader();

    // Raft can not step request snapshot if itself is a leader.
    result = r->RequestSnapshot();
    CHECK(!result);

    // Advance matched.
    Message m;
    auto builder = m.builder();
    builder.setMsgType(MessageType::MSG_APPEND_RESPONSE);
    builder.setFrom(2);
    builder.setTo(1);
    builder.setIndex(11);
    auto res = r.Step(m);
    (void)res;
    auto& voter_2 = r->progress_tracker().progress_map().at(2);
    CHECK_EQ(voter_2.state(), ProgressState::Replicate);

    uint64_t request_snapshot_idx = r->raft_log().committed();
    builder.setIndex(11);
    builder.setReject(true);
    builder.setRejectHint(INVALID_INDEX);
    builder.setRequestSnapshot(request_snapshot_idx);

    // Ignore out of order request snapshot messages.
    Message out_of_order = m.clone();
    out_of_order.builder().setIndex(9);
    res = r.Step(out_of_order);
    (void)res;
    CHECK_EQ(voter_2.state(), ProgressState::Replicate);

    // Clear messages from previous steps (BecomeLeader, MsgAppendResponse handling).
    r.ReadMessages();

    // Request snapshot.
    res = r.Step(m);
    (void)res;
    CHECK_EQ(voter_2.state(), ProgressState::Snapshot);
    CHECK_EQ(voter_2.pending_snapshot(), 11);
    CHECK_EQ(voter_2.next_idx(), 12);
    CHECK(voter_2.IsPaused());

    auto msgs = r.ReadMessages();
    CHECK_EQ(msgs.size(), 1);
    auto msg_reader = msgs[0].reader();
    CHECK_EQ(msg_reader.getMsgType(), MessageType::MSG_SNAPSHOT);
    CHECK_EQ(msg_reader.getSnapshot().getMetadata().getIndex(), request_snapshot_idx);

    // Append/heartbeats does not set the state from snapshot to probe.
    builder.setMsgType(MessageType::MSG_APPEND_RESPONSE);
    builder.setIndex(11);
    res = r.Step(m);
    (void)res;
    CHECK_EQ(voter_2.state(), ProgressState::Snapshot);
    CHECK_EQ(voter_2.pending_snapshot(), 11);
    CHECK_EQ(voter_2.next_idx(), 12);
    CHECK(voter_2.IsPaused());

    // However snapshot status report does set the stat to probe.
    builder.setMsgType(MessageType::MSG_SNAP_STATUS);
    res = r.Step(m);
    (void)res;
    CHECK_EQ(voter_2.state(), ProgressState::Probe);
    CHECK_EQ(voter_2.pending_snapshot(), 0);
    CHECK_EQ(voter_2.next_idx(), 12);
    CHECK(voter_2.IsPaused());
}

TEST_SUITE_END();
