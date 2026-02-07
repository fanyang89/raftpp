#include "raftpp/core/read_only.h"

#include <doctest/doctest.h>

#include "raftpp/core/capnp_util.h"
#include "raftpp/core/types.h"
#include "test_util.h"

using namespace raftpp;

namespace {

// Create a ReadIndex message with the given ctx stored in an entry's data field
Message NewReadIndexMessage(uint64_t from, uint64_t to, const std::string& ctx) {
    std::vector<Entry> entries;
    entries.push_back(NewEntry(0, 0, ctx));
    return NewMessageWithEntries(from, to, MessageType::MSG_READ_INDEX, std::move(entries));
}

// Helper to get the ctx from a ReadIndexStatus
std::string GetCtxFromStatus(const ReadIndexStatus& status) {
    auto req_reader = capnp_util::reader<msg::Message>(status.req);
    auto entries = req_reader.getEntries();
    if (entries.size() == 0) {
        return "";
    }
    auto data = entries[0].getData();
    return std::string(reinterpret_cast<const char*>(data.begin()), data.size());
}

}  // namespace

TEST_SUITE_BEGIN("ReadOnly");

// ============================================================================
// Construction Tests
// ============================================================================

TEST_CASE("ReadOnly: construction with Safe option") {
    ReadOnly ro(ReadOnlyOption::Safe);
    CHECK_EQ(ro.option(), ReadOnlyOption::Safe);
    CHECK_EQ(ro.PendingReadCount(), 0);
    CHECK_EQ(ro.LastPendingRequestCtx(), std::nullopt);
}

TEST_CASE("ReadOnly: construction with LeaseBased option") {
    ReadOnly ro(ReadOnlyOption::LeaseBased);
    CHECK_EQ(ro.option(), ReadOnlyOption::LeaseBased);
    CHECK_EQ(ro.PendingReadCount(), 0);
    CHECK_EQ(ro.LastPendingRequestCtx(), std::nullopt);
}

// ============================================================================
// AddRequest Tests
// ============================================================================

TEST_CASE("ReadOnly: AddRequest basic") {
    ReadOnly ro(ReadOnlyOption::Safe);
    const uint64_t self_id = 1;
    const uint64_t index = 10;
    const std::string ctx = "ctx1";

    Message msg = NewReadIndexMessage(2, 1, ctx);
    ro.AddRequest(index, msg, self_id);

    CHECK_EQ(ro.PendingReadCount(), 1);
    CHECK_EQ(ro.LastPendingRequestCtx(), ctx);
}

TEST_CASE("ReadOnly: AddRequest multiple") {
    ReadOnly ro(ReadOnlyOption::Safe);
    const uint64_t self_id = 1;

    Message msg1 = NewReadIndexMessage(2, 1, "ctx1");
    Message msg2 = NewReadIndexMessage(3, 1, "ctx2");
    Message msg3 = NewReadIndexMessage(4, 1, "ctx3");

    ro.AddRequest(10, msg1, self_id);
    ro.AddRequest(11, msg2, self_id);
    ro.AddRequest(12, msg3, self_id);

    CHECK_EQ(ro.PendingReadCount(), 3);
    CHECK_EQ(ro.LastPendingRequestCtx(), "ctx3");
}

TEST_CASE("ReadOnly: AddRequest duplicate ctx ignored") {
    ReadOnly ro(ReadOnlyOption::Safe);
    const uint64_t self_id = 1;
    const std::string ctx = "ctx1";

    Message msg1 = NewReadIndexMessage(2, 1, ctx);
    Message msg2 = NewReadIndexMessage(3, 1, ctx);

    ro.AddRequest(10, msg1, self_id);
    ro.AddRequest(20, msg2, self_id);  // Same ctx, should be ignored

    CHECK_EQ(ro.PendingReadCount(), 1);

    // Verify the original request is kept (index=10)
    auto result = ro.Advance(ctx);
    REQUIRE_EQ(result.size(), 1);
    CHECK_EQ(result[0].index, 10);
}

TEST_CASE("ReadOnly: AddRequest empty entries ignored") {
    ReadOnly ro(ReadOnlyOption::Safe);
    const uint64_t self_id = 1;

    // Create a message with no entries
    Message msg = NewMessageWithEntries(2, 1, MessageType::MSG_READ_INDEX, {});
    ro.AddRequest(10, msg, self_id);

    CHECK_EQ(ro.PendingReadCount(), 0);
    CHECK_EQ(ro.LastPendingRequestCtx(), std::nullopt);
}

// ============================================================================
// LastPendingRequestCtx Tests
// ============================================================================

TEST_CASE("ReadOnly: LastPendingRequestCtx empty") {
    ReadOnly ro(ReadOnlyOption::Safe);
    CHECK_EQ(ro.LastPendingRequestCtx(), std::nullopt);
}

TEST_CASE("ReadOnly: LastPendingRequestCtx single") {
    ReadOnly ro(ReadOnlyOption::Safe);
    const std::string ctx = "single_ctx";

    Message msg = NewReadIndexMessage(2, 1, ctx);
    ro.AddRequest(10, msg, 1);

    CHECK_EQ(ro.LastPendingRequestCtx(), ctx);
}

TEST_CASE("ReadOnly: LastPendingRequestCtx multiple") {
    ReadOnly ro(ReadOnlyOption::Safe);

    ro.AddRequest(10, NewReadIndexMessage(2, 1, "ctx1"), 1);
    ro.AddRequest(11, NewReadIndexMessage(2, 1, "ctx2"), 1);
    ro.AddRequest(12, NewReadIndexMessage(2, 1, "ctx3"), 1);

    CHECK_EQ(ro.LastPendingRequestCtx(), "ctx3");
}

// ============================================================================
// PendingReadCount Tests
// ============================================================================

TEST_CASE("ReadOnly: PendingReadCount empty") {
    ReadOnly ro(ReadOnlyOption::Safe);
    CHECK_EQ(ro.PendingReadCount(), 0);
}

TEST_CASE("ReadOnly: PendingReadCount after add") {
    ReadOnly ro(ReadOnlyOption::Safe);

    CHECK_EQ(ro.PendingReadCount(), 0);

    ro.AddRequest(10, NewReadIndexMessage(2, 1, "ctx1"), 1);
    CHECK_EQ(ro.PendingReadCount(), 1);

    ro.AddRequest(11, NewReadIndexMessage(2, 1, "ctx2"), 1);
    CHECK_EQ(ro.PendingReadCount(), 2);

    ro.AddRequest(12, NewReadIndexMessage(2, 1, "ctx3"), 1);
    CHECK_EQ(ro.PendingReadCount(), 3);
}

TEST_CASE("ReadOnly: PendingReadCount after advance") {
    ReadOnly ro(ReadOnlyOption::Safe);

    ro.AddRequest(10, NewReadIndexMessage(2, 1, "ctx1"), 1);
    ro.AddRequest(11, NewReadIndexMessage(2, 1, "ctx2"), 1);
    ro.AddRequest(12, NewReadIndexMessage(2, 1, "ctx3"), 1);
    CHECK_EQ(ro.PendingReadCount(), 3);

    std::ignore = ro.Advance("ctx1");
    CHECK_EQ(ro.PendingReadCount(), 2);

    std::ignore = ro.Advance("ctx3");
    CHECK_EQ(ro.PendingReadCount(), 0);
}

// ============================================================================
// RecvACK Tests
// ============================================================================

TEST_CASE("ReadOnly: RecvACK unknown ctx") {
    ReadOnly ro(ReadOnlyOption::Safe);

    auto acks = ro.RecvACK(2, "unknown_ctx");
    CHECK_EQ(acks, std::nullopt);
}

TEST_CASE("ReadOnly: RecvACK single") {
    ReadOnly ro(ReadOnlyOption::Safe);
    const uint64_t self_id = 1;
    const std::string ctx = "ctx1";

    ro.AddRequest(10, NewReadIndexMessage(2, 1, ctx), self_id);

    // Receive ACK from node 2
    auto acks = ro.RecvACK(2, ctx);
    REQUIRE(acks.has_value());
    CHECK(acks->contains(self_id));  // self_id is always in acks
    CHECK(acks->contains(2));
    CHECK_EQ(acks->size(), 2);
}

TEST_CASE("ReadOnly: RecvACK multiple from same node idempotent") {
    ReadOnly ro(ReadOnlyOption::Safe);
    const uint64_t self_id = 1;
    const std::string ctx = "ctx1";

    ro.AddRequest(10, NewReadIndexMessage(2, 1, ctx), self_id);

    auto acks1 = ro.RecvACK(2, ctx);
    auto acks2 = ro.RecvACK(2, ctx);  // Duplicate ACK
    auto acks3 = ro.RecvACK(2, ctx);  // Another duplicate

    REQUIRE(acks3.has_value());
    CHECK_EQ(acks3->size(), 2);  // Still only 2: self_id and node 2
}

TEST_CASE("ReadOnly: RecvACK from multiple nodes") {
    ReadOnly ro(ReadOnlyOption::Safe);
    const uint64_t self_id = 1;
    const std::string ctx = "ctx1";

    ro.AddRequest(10, NewReadIndexMessage(2, 1, ctx), self_id);

    std::ignore = ro.RecvACK(2, ctx);
    std::ignore = ro.RecvACK(3, ctx);
    auto acks = ro.RecvACK(4, ctx);

    REQUIRE(acks.has_value());
    CHECK(acks->contains(self_id));
    CHECK(acks->contains(2));
    CHECK(acks->contains(3));
    CHECK(acks->contains(4));
    CHECK_EQ(acks->size(), 4);
}

// ============================================================================
// Advance Tests
// ============================================================================

TEST_CASE("ReadOnly: Advance unknown ctx") {
    ReadOnly ro(ReadOnlyOption::Safe);

    auto result = ro.Advance("unknown_ctx");
    CHECK(result.empty());
}

TEST_CASE("ReadOnly: Advance single") {
    ReadOnly ro(ReadOnlyOption::Safe);
    const uint64_t self_id = 1;
    const std::string ctx = "ctx1";

    ro.AddRequest(10, NewReadIndexMessage(2, 1, ctx), self_id);
    std::ignore = ro.RecvACK(2, ctx);
    std::ignore = ro.RecvACK(3, ctx);

    auto result = ro.Advance(ctx);
    REQUIRE_EQ(result.size(), 1);
    CHECK_EQ(result[0].index, 10);
    CHECK_EQ(GetCtxFromStatus(result[0]), ctx);
    CHECK(result[0].acks.contains(self_id));
    CHECK(result[0].acks.contains(2));
    CHECK(result[0].acks.contains(3));

    // After advance, pending count should be 0
    CHECK_EQ(ro.PendingReadCount(), 0);
}

TEST_CASE("ReadOnly: Advance multiple batch clear") {
    ReadOnly ro(ReadOnlyOption::Safe);
    const uint64_t self_id = 1;

    ro.AddRequest(10, NewReadIndexMessage(2, 1, "ctx1"), self_id);
    ro.AddRequest(11, NewReadIndexMessage(2, 1, "ctx2"), self_id);
    ro.AddRequest(12, NewReadIndexMessage(2, 1, "ctx3"), self_id);

    // Advance to ctx3 should clear all three
    auto result = ro.Advance("ctx3");
    REQUIRE_EQ(result.size(), 3);
    CHECK_EQ(result[0].index, 10);
    CHECK_EQ(result[1].index, 11);
    CHECK_EQ(result[2].index, 12);

    CHECK_EQ(ro.PendingReadCount(), 0);
}

TEST_CASE("ReadOnly: Advance partial") {
    ReadOnly ro(ReadOnlyOption::Safe);
    const uint64_t self_id = 1;

    ro.AddRequest(10, NewReadIndexMessage(2, 1, "ctx1"), self_id);
    ro.AddRequest(11, NewReadIndexMessage(2, 1, "ctx2"), self_id);
    ro.AddRequest(12, NewReadIndexMessage(2, 1, "ctx3"), self_id);
    ro.AddRequest(13, NewReadIndexMessage(2, 1, "ctx4"), self_id);

    // Advance to ctx2 should only clear ctx1 and ctx2
    auto result = ro.Advance("ctx2");
    REQUIRE_EQ(result.size(), 2);
    CHECK_EQ(result[0].index, 10);
    CHECK_EQ(result[1].index, 11);

    CHECK_EQ(ro.PendingReadCount(), 2);
    CHECK_EQ(ro.LastPendingRequestCtx(), "ctx4");
}

TEST_CASE("ReadOnly: Advance order preserved") {
    ReadOnly ro(ReadOnlyOption::Safe);
    const uint64_t self_id = 1;

    // Add in specific order
    ro.AddRequest(100, NewReadIndexMessage(2, 1, "alpha"), self_id);
    ro.AddRequest(200, NewReadIndexMessage(2, 1, "beta"), self_id);
    ro.AddRequest(300, NewReadIndexMessage(2, 1, "gamma"), self_id);
    ro.AddRequest(400, NewReadIndexMessage(2, 1, "delta"), self_id);

    auto result = ro.Advance("delta");
    REQUIRE_EQ(result.size(), 4);

    // Verify order matches addition order
    CHECK_EQ(GetCtxFromStatus(result[0]), "alpha");
    CHECK_EQ(result[0].index, 100);

    CHECK_EQ(GetCtxFromStatus(result[1]), "beta");
    CHECK_EQ(result[1].index, 200);

    CHECK_EQ(GetCtxFromStatus(result[2]), "gamma");
    CHECK_EQ(result[2].index, 300);

    CHECK_EQ(GetCtxFromStatus(result[3]), "delta");
    CHECK_EQ(result[3].index, 400);
}

// ============================================================================
// Integration Tests
// ============================================================================

TEST_CASE("ReadOnly: full workflow") {
    ReadOnly ro(ReadOnlyOption::Safe);
    const uint64_t leader_id = 1;
    const uint64_t node2 = 2;
    const uint64_t node3 = 3;
    const std::string ctx = "read_request_1";
    const uint64_t commit_index = 100;

    // Step 1: Leader receives read request, adds to pending
    Message read_req = NewReadIndexMessage(node2, leader_id, ctx);
    ro.AddRequest(commit_index, read_req, leader_id);

    CHECK_EQ(ro.PendingReadCount(), 1);
    CHECK_EQ(ro.LastPendingRequestCtx(), ctx);

    // Step 2: Leader broadcasts heartbeat, receives ACKs
    auto acks1 = ro.RecvACK(node2, ctx);
    REQUIRE(acks1.has_value());
    CHECK_EQ(acks1->size(), 2);  // leader_id + node2

    auto acks2 = ro.RecvACK(node3, ctx);
    REQUIRE(acks2.has_value());
    CHECK_EQ(acks2->size(), 3);  // leader_id + node2 + node3

    // Step 3: Quorum achieved (3 out of 3), advance read
    auto result = ro.Advance(ctx);
    REQUIRE_EQ(result.size(), 1);
    CHECK_EQ(result[0].index, commit_index);
    CHECK_EQ(result[0].acks.size(), 3);

    // Step 4: Verify cleanup
    CHECK_EQ(ro.PendingReadCount(), 0);
    CHECK_EQ(ro.LastPendingRequestCtx(), std::nullopt);
}

TEST_CASE("ReadOnly: concurrent requests") {
    ReadOnly ro(ReadOnlyOption::Safe);
    const uint64_t leader_id = 1;

    // Multiple clients send read requests concurrently
    ro.AddRequest(100, NewReadIndexMessage(2, 1, "client1_req"), leader_id);
    ro.AddRequest(100, NewReadIndexMessage(3, 1, "client2_req"), leader_id);
    ro.AddRequest(101, NewReadIndexMessage(4, 1, "client3_req"), leader_id);

    CHECK_EQ(ro.PendingReadCount(), 3);

    // Receive ACKs for all requests
    std::ignore = ro.RecvACK(2, "client1_req");
    std::ignore = ro.RecvACK(3, "client1_req");

    std::ignore = ro.RecvACK(2, "client2_req");
    std::ignore = ro.RecvACK(3, "client2_req");

    std::ignore = ro.RecvACK(2, "client3_req");
    std::ignore = ro.RecvACK(3, "client3_req");

    // Advance client2_req (middle one)
    auto result = ro.Advance("client2_req");
    REQUIRE_EQ(result.size(), 2);  // client1 and client2
    CHECK_EQ(GetCtxFromStatus(result[0]), "client1_req");
    CHECK_EQ(GetCtxFromStatus(result[1]), "client2_req");

    CHECK_EQ(ro.PendingReadCount(), 1);
    CHECK_EQ(ro.LastPendingRequestCtx(), "client3_req");

    // Advance remaining
    auto result2 = ro.Advance("client3_req");
    REQUIRE_EQ(result2.size(), 1);
    CHECK_EQ(GetCtxFromStatus(result2[0]), "client3_req");
    CHECK_EQ(ro.PendingReadCount(), 0);
}

TEST_CASE("ReadOnly: advance after partial acks") {
    ReadOnly ro(ReadOnlyOption::Safe);
    const uint64_t leader_id = 1;
    const std::string ctx = "ctx1";

    ro.AddRequest(50, NewReadIndexMessage(2, 1, ctx), leader_id);

    // Only partial ACKs received
    std::ignore = ro.RecvACK(2, ctx);
    // Node 3 hasn't ACKed yet

    // Can still advance and get what we have
    auto result = ro.Advance(ctx);
    REQUIRE_EQ(result.size(), 1);
    CHECK_EQ(result[0].acks.size(), 2);  // leader_id + node2

    CHECK_EQ(ro.PendingReadCount(), 0);
}

TEST_SUITE_END();
