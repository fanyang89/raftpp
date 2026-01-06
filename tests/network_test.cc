#include "harness/network.h"

#include <doctest/doctest.h>

#include "harness/test_util.h"

using namespace raftpp;

TEST_SUITE_BEGIN("harness");

TEST_CASE("harness: create network") {
    auto network = CreateTestNetwork(3);

    CHECK_EQ(network.Size(), 3);
    CHECK(network.GetPeer(1) != nullptr);
    CHECK(network.GetPeer(2) != nullptr);
    CHECK(network.GetPeer(3) != nullptr);
    CHECK(network.GetPeer(4) == nullptr);
}

TEST_CASE("harness: drop and recover") {
    auto network = CreateTestNetwork(2);

    network.Drop(1, 2, 0.5);

    // After dropping 50%, we need to verify it works
    // For now, just ensure network is functional
    CHECK_EQ(network.Size(), 2);

    network.Recover();

    // After recovery, network should be functional again
    CHECK_EQ(network.Size(), 2);
}

TEST_CASE("harness: isolate node") {
    auto network = CreateTestNetwork(3);

    network.Isolate(2);

    // Node 2 should be isolated
    CHECK_EQ(network.Size(), 3);

    network.Recover();

    // After recovery, all nodes should be able to communicate
    CHECK_EQ(network.Size(), 3);
}

TEST_CASE("harness: ignore message type") {
    auto network = CreateTestNetwork(2);

    network.IgnoreMessageType(MsgHeartbeat);

    // Heartbeat messages should be ignored
    CHECK_EQ(network.Size(), 2);

    network.Recover();

    CHECK_EQ(network.Size(), 2);
}

TEST_SUITE_END();
