// Port of raft-rs harness/tests/integration_cases/test_raft_flow_control.rs
// Flow control tests.

#include <doctest/doctest.h>
#include <kj/array.h>

#include "harness/network.h"
#include "harness/test_util.h"

using namespace raftpp;

namespace {

Message MakeProposeMessage() {
    Entry entry = NewEntry(0, 0, "test");
    return NewMessageWithEntries(1, 1, MessageType::MSG_PROPOSE, std::vector<Entry>{entry});
}

Message MakeAppendResponse(uint64_t index) {
    Message m;
    auto builder = m.builder();
    builder.setMsgType(MessageType::MSG_APPEND_RESPONSE);
    builder.setFrom(2);
    builder.setTo(1);
    builder.setIndex(index);
    return m;
}

Message MakeHeartbeatResponse() {
    Message m;
    auto builder = m.builder();
    builder.setMsgType(MessageType::MSG_HEARTBEAT_RESPONSE);
    builder.setFrom(2);
    builder.setTo(1);
    return m;
}

}  // namespace

TEST_SUITE_BEGIN("raft_flow_control");

// test_msg_app_flow_control_full ensures:
// 1. msgApp can fill the sending window until full
// 2. when the window is full, no more msgApp can be sent.
TEST_CASE("msg app flow control full") {
    auto storage = std::make_shared<MemoryStorage>();
    auto r = NewTestRaft(1, {1, 2}, 5, 1, storage);
    r->BecomeCandidate();
    r->BecomeLeader();

    // force the progress to be in replicate state
    auto& pr = r->progress_tracker().progress_map().at(2);
    pr.BecomeReplicate();

    // fill in the inflights window
    for (size_t i = 0; i < r->max_inflight_messages(); ++i) {
        Message m = MakeProposeMessage();
        auto result = r.Step(m);
        (void)result;

        auto ms = r.ReadMessages();
        CHECK_EQ(ms.size(), 1);
    }

    // ensure 1
    CHECK(pr.inflights().Full());

    // ensure 2
    for (size_t i = 0; i < 10; ++i) {
        Message m = MakeProposeMessage();
        auto result = r.Step(m);
        (void)result;

        auto ms = r.ReadMessages();
        CHECK(ms.empty());
    }
}

// test_msg_app_flow_control_move_forward ensures msgAppResp can move
// forward the sending window correctly:
// 1. valid msgAppResp.index moves the windows to pass all smaller or equal index.
// 2. out-of-dated msgAppResp has no effect on the sliding window.
TEST_CASE("msg app flow control move forward") {
    auto storage = std::make_shared<MemoryStorage>();
    auto r = NewTestRaft(1, {1, 2}, 5, 1, storage);
    r->BecomeCandidate();
    r->BecomeLeader();

    // force the progress to be in replicate state
    auto& pr = r->progress_tracker().progress_map().at(2);
    pr.BecomeReplicate();

    // fill in the inflights window
    for (size_t i = 0; i < r->max_inflight_messages(); ++i) {
        Message m = MakeProposeMessage();
        auto result = r.Step(m);
        (void)result;
        r.ReadMessages();
    }

    // 1 is noop, 2 is the first proposal we just sent.
    // so we start with 2.
    for (size_t tt = 2; tt < r->max_inflight_messages(); ++tt) {
        // move forward the window
        Message m = MakeAppendResponse(tt);
        auto result = r.Step(m);
        (void)result;
        r.ReadMessages();

        // fill in the inflights window again
        Message m2 = MakeProposeMessage();
        result = r.Step(m2);
        (void)result;
        auto ms = r.ReadMessages();
        CHECK_EQ(ms.size(), 1);

        // ensure 1
        CHECK(pr.inflights().Full());

        // ensure 2
        for (size_t i = 0; i < tt; ++i) {
            Message m3 = MakeAppendResponse(i);
            result = r.Step(m3);
            (void)result;

            CHECK(pr.inflights().Full());
        }
    }
}

// test_msg_app_flow_control_recv_heartbeat ensures a heartbeat response
// frees one slot if the window is full.
TEST_CASE("msg app flow control recv heartbeat") {
    auto storage = std::make_shared<MemoryStorage>();
    auto r = NewTestRaft(1, {1, 2}, 5, 1, storage);
    r->BecomeCandidate();
    r->BecomeLeader();

    // force the progress to be in replicate state
    auto& pr = r->progress_tracker().progress_map().at(2);
    pr.BecomeReplicate();

    // fill in the inflights window
    for (size_t i = 0; i < r->max_inflight_messages(); ++i) {
        Message m = MakeProposeMessage();
        auto result = r.Step(m);
        (void)result;
        r.ReadMessages();
    }

    for (size_t tt = 1; tt < 5; ++tt) {
        CHECK(pr.inflights().Full());

        // recv tt MessageType::MSG_HEARTBEAT_RESPONSE and expect one free slot
        for (size_t i = 0; i < tt; ++i) {
            Message m = MakeHeartbeatResponse();
            auto result = r.Step(m);
            (void)result;
            r.ReadMessages();
            CHECK(!pr.inflights().Full());
        }

        // one slot
        Message m = MakeProposeMessage();
        auto result = r.Step(m);
        (void)result;
        auto ms = r.ReadMessages();
        CHECK_EQ(ms.size(), 1);

        // and just one slot
        for (size_t i = 0; i < 10; ++i) {
            Message m2 = MakeProposeMessage();
            result = r.Step(m2);
            (void)result;
            auto ms1 = r.ReadMessages();
            CHECK(ms1.empty());
        }

        // clear all pending messages
        Message m3 = MakeHeartbeatResponse();
        result = r.Step(m3);
        (void)result;
        r.ReadMessages();
    }
}

TEST_CASE("msg app flow control with freeing resources") {
    auto storage = std::make_shared<MemoryStorage>();
    auto r = NewTestRaft(1, {1, 2, 3}, 5, 1, storage);

    r->BecomeCandidate();
    r->BecomeLeader();

    for (const auto& [id, pr] : r->progress_tracker().progress_map()) {
        CHECK(!pr.inflights().buffer_is_allocated());
    }

    for (uint64_t i = 1; i <= 3; ++i) {
        // Force the progress to be in replicate state.
        auto& pr = r->progress_tracker().progress_map().at(i);
        pr.BecomeReplicate();
    }

    Message m = MakeProposeMessage();
    auto result = r.Step(m);
    (void)result;

    for (const auto& [id, pr] : r->progress_tracker().progress_map()) {
        if (id != 1) {
            CHECK(pr.inflights().buffer_is_allocated());
            CHECK_EQ(pr.inflights().Count(), 1);
        }
    }

    /*
    1: cap=0/start=0/count=0/buffer=[]
    2: cap=256/start=0/count=1/buffer=[2]
    3: cap=256/start=0/count=1/buffer=[2]
    */

    Message resp = MakeAppendResponse(r->raft_log().LastIndex());
    result = r.Step(resp);
    (void)result;

    CHECK_EQ(r->progress_tracker().progress_map().at(2).inflights().Count(), 0);

    /*
    1: cap=0/start=0/count=0/buffer=[]
    2: cap=256/start=1/count=0/buffer=[2]
    3: cap=256/start=0/count=1/buffer=[2]
    */

    Message m2 = MakeProposeMessage();
    result = r.Step(m2);
    (void)result;

    CHECK_EQ(r->progress_tracker().progress_map().at(2).inflights().Count(), 1);
    CHECK_EQ(r->progress_tracker().progress_map().at(3).inflights().Count(), 2);

    /*
    1: cap=0/start=0/count=0/buffer=[]
    2: cap=256/start=1/count=1/buffer=[2,3]
    3: cap=256/start=0/count=2/buffer=[2,3]
    */

    Message resp2 = MakeAppendResponse(r->raft_log().LastIndex());
    result = r.Step(resp2);
    (void)result;

    CHECK_EQ(r->progress_tracker().progress_map().at(2).inflights().Count(), 0);
    CHECK_EQ(r->progress_tracker().progress_map().at(3).inflights().Count(), 2);
    CHECK_EQ(r->inflight_buffers_size(), 4096);

    /*
    1: cap=0/start=0/count=0/buffer=[]
    2: cap=256/start=2/count=0/buffer=[2,3]
    3: cap=256/start=0/count=2/buffer=[2,3]
    */

    r->maybe_free_inflight_buffers();

    CHECK(!r->progress_tracker().progress_map().at(2).inflights().buffer_is_allocated());
    CHECK_EQ(r->progress_tracker().progress_map().at(2).inflights().Count(), 0);
    CHECK_EQ(r->inflight_buffers_size(), 2048);

    /*
    1: cap=0/start=0/count=0/buffer=[]
    2: cap=0/start=0/count=0/buffer=[]
    3: cap=256/start=0/count=2/buffer=[2,3]
    */
}

// Test progress can be disabled with `adjust_max_inflight_msgs(<id>, 0)`.
TEST_CASE("disable progress") {
    auto storage = std::make_shared<MemoryStorage>();
    auto r = NewTestRaft(1, {1, 2}, 5, 1, storage);
    r->BecomeCandidate();
    r->BecomeLeader();

    auto& pr = r->progress_tracker().progress_map().at(2);
    pr.BecomeReplicate();

    // Disable the progress 2. Internal `free`s shouldn't fail.
    r->adjust_max_inflight_msgs(2, 0);
    Message m = MakeHeartbeatResponse();
    auto result = r.Step(m);
    (void)result;

    CHECK(pr.inflights().Full());
    CHECK_EQ(pr.inflights().Count(), 0);

    // Progress 2 is disabled.
    auto msgs = r.ReadMessages();
    CHECK_EQ(msgs.size(), 0);

    // After the progress gets enabled and a heartbeat response is received,
    // its leader can continue to append entries to it.
    r->adjust_max_inflight_msgs(2, 10);
    Message m2 = MakeHeartbeatResponse();
    result = r.Step(m2);
    (void)result;
    msgs = r.ReadMessages();
    CHECK_EQ(msgs.size(), 1);
    CHECK_EQ(msgs[0].reader().getMsgType(), MessageType::MSG_APPEND);
}

TEST_SUITE_END();
