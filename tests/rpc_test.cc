#include <doctest/doctest.h>

#include "raftpp/rpc/codec.h"
#include "raftpp/rpc/peer_manager.h"

using namespace raftpp;
using namespace raftpp::rpc;

TEST_SUITE("rpc") {

TEST_CASE("Codec encode/decode round-trip") {
    Message msg;
    msg.set_msg_type(MsgAppend);
    msg.set_from(1);
    msg.set_to(2);
    msg.set_term(5);
    msg.set_index(100);
    msg.set_commit(50);

    auto encoded = Codec::Encode(msg, 1, 2, 42);

    // Check minimum size (prefix + header + payload)
    CHECK(encoded.size() >= Codec::kPrefixSize);

    auto result = Codec::Decode(encoded, Codec::kDefaultMaxMessageSize);
    REQUIRE(result.has_value());

    auto& decode_result = *result;
    CHECK(decode_result.bytes_consumed == encoded.size());
    CHECK(decode_result.message.msg_type() == msg.msg_type());
    CHECK(decode_result.message.from() == msg.from());
    CHECK(decode_result.message.to() == msg.to());
    CHECK(decode_result.message.term() == msg.term());
    CHECK(decode_result.message.index() == msg.index());
    CHECK(decode_result.message.commit() == msg.commit());

    // Check RpcHeader fields
    CHECK(decode_result.header.version() == Codec::kVersion);
    CHECK(decode_result.header.from_node() == 1);
    CHECK(decode_result.header.to_node() == 2);
    CHECK(decode_result.header.request_id() == 42);
    CHECK(decode_result.header.msg_type() == MsgAppend);
}

TEST_CASE("Codec handles incomplete buffer") {
    Message msg;
    msg.set_msg_type(MsgHeartbeat);
    msg.set_from(1);
    msg.set_to(2);

    auto encoded = Codec::Encode(msg);

    // Only provide partial prefix
    std::span<const uint8_t> partial(encoded.data(), Codec::kPrefixSize / 2);
    auto result = Codec::Decode(partial, Codec::kDefaultMaxMessageSize);
    REQUIRE(result.has_value());
    CHECK(result->bytes_consumed == 0);  // Should return 0 bytes consumed

    // Provide prefix but not full header/payload
    std::span<const uint8_t> prefix_only(encoded.data(), Codec::kPrefixSize + 1);
    result = Codec::Decode(prefix_only, Codec::kDefaultMaxMessageSize);
    REQUIRE(result.has_value());
    CHECK(result->bytes_consumed == 0);
}

TEST_CASE("Codec rejects invalid magic") {
    std::vector<uint8_t> bad_magic = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    auto result = Codec::Decode(bad_magic, Codec::kDefaultMaxMessageSize);
    REQUIRE(!result.has_value());
    CHECK(result.error().code == RpcErrorCode::InvalidMagic);
}

TEST_CASE("Codec rejects oversized message") {
    Message msg;
    msg.set_msg_type(MsgSnapshot);
    msg.set_from(1);
    msg.set_to(2);

    auto encoded = Codec::Encode(msg);

    // Try to decode with very small max size
    auto result = Codec::Decode(encoded, 1);  // 1 byte max
    REQUIRE(!result.has_value());
    CHECK(result.error().code == RpcErrorCode::MessageTooLarge);
}

TEST_CASE("Codec handles message with entries") {
    Message msg;
    msg.set_msg_type(MsgAppend);
    msg.set_from(1);
    msg.set_to(2);
    msg.set_term(3);

    // Add entries
    for (int i = 0; i < 10; i++) {
        auto* entry = msg.add_entries();
        entry->set_term(3);
        entry->set_index(i + 1);
        entry->set_data("test data " + std::to_string(i));
    }

    auto encoded = Codec::Encode(msg);
    auto result = Codec::Decode(encoded, Codec::kDefaultMaxMessageSize);
    REQUIRE(result.has_value());

    auto& decode_result = *result;
    CHECK(decode_result.message.entries_size() == 10);
    CHECK(decode_result.message.entries(5).data() == "test data 5");
}

TEST_CASE("Handshake encode/decode round-trip") {
    Handshake hs;
    hs.node_id = 12345;
    hs.cluster_id = 999;

    auto encoded = hs.Encode();
    CHECK(encoded.size() >= Handshake::kSize);  // At least prefix size

    auto result = Handshake::Decode(encoded);
    REQUIRE(result.has_value());
    CHECK(result->node_id == 12345);
    CHECK(result->cluster_id == 999);
}

TEST_CASE("HandshakeCodec encode/decode round-trip") {
    RpcHandshake hs;
    hs.set_version(1);
    hs.set_node_id(54321);
    hs.set_cluster_id(888);

    auto encoded = HandshakeCodec::Encode(hs);
    CHECK(encoded.size() >= HandshakeCodec::kPrefixSize);

    auto result = HandshakeCodec::Decode(encoded);
    REQUIRE(result.has_value());
    auto& [decoded, consumed] = *result;
    CHECK(consumed == encoded.size());
    CHECK(decoded.version() == 1);
    CHECK(decoded.node_id() == 54321);
    CHECK(decoded.cluster_id() == 888);
}

TEST_CASE("Handshake rejects invalid magic") {
    std::vector<uint8_t> bad_hs(HandshakeCodec::kPrefixSize, 0);

    auto result = Handshake::Decode(bad_hs);
    REQUIRE(!result.has_value());
    CHECK(result.error().code == RpcErrorCode::InvalidMagic);
}

TEST_CASE("Handshake rejects incomplete buffer") {
    std::vector<uint8_t> short_buf(HandshakeCodec::kPrefixSize - 1, 0);

    auto result = HandshakeCodec::Decode(short_buf);
    REQUIRE(result.has_value());
    CHECK(result->second == 0);  // Incomplete, returns 0 bytes consumed
}

TEST_CASE("PeerManager basic operations") {
    PeerManager pm;

    pm.AddPeer(1, "192.168.1.1:9000");
    pm.AddPeer(2, "192.168.1.2:9000");
    pm.AddPeer(3, "192.168.1.3:9000");

    CHECK(pm.Size() == 3);
    CHECK(pm.HasPeer(1));
    CHECK(pm.HasPeer(2));
    CHECK(pm.HasPeer(3));
    CHECK(!pm.HasPeer(4));

    auto* peer = pm.GetPeer(1);
    REQUIRE(peer != nullptr);
    CHECK(peer->id == 1);
    CHECK(peer->addr == "192.168.1.1:9000");
    CHECK(peer->state == PeerState::Disconnected);

    pm.RemovePeer(2);
    CHECK(pm.Size() == 2);
    CHECK(!pm.HasPeer(2));
}

TEST_CASE("PeerManager state transitions") {
    PeerManager pm;
    pm.AddPeer(1, "127.0.0.1:9000");

    CHECK(pm.ConnectedCount() == 0);

    pm.UpdateState(1, PeerState::Connecting);
    CHECK(pm.GetPeer(1)->state == PeerState::Connecting);
    CHECK(pm.ConnectedCount() == 0);

    pm.UpdateState(1, PeerState::Connected);
    CHECK(pm.GetPeer(1)->state == PeerState::Connected);
    CHECK(pm.ConnectedCount() == 1);

    pm.UpdateState(1, PeerState::Disconnected);
    CHECK(pm.GetPeer(1)->state == PeerState::Disconnected);
    CHECK(pm.ConnectedCount() == 0);
}

TEST_CASE("PeerManager reconnection with backoff") {
    PeerManager pm;
    pm.AddPeer(1, "127.0.0.1:9000");

    // Initially, peer should be ready for reconnection
    auto peers = pm.GetPeersToReconnect();
    CHECK(peers.size() == 1);

    // Record failure - should delay reconnection
    pm.RecordFailure(1);
    peers = pm.GetPeersToReconnect();
    CHECK(peers.empty());  // Not yet time to reconnect

    // After failure, state should be disconnected
    CHECK(pm.GetPeer(1)->state == PeerState::Disconnected);
    CHECK(pm.GetPeer(1)->failure_count == 1);
}

TEST_CASE("PeerManager GetAllPeerIds") {
    PeerManager pm;
    pm.AddPeer(1, "a");
    pm.AddPeer(2, "b");
    pm.AddPeer(3, "c");

    auto ids = pm.GetAllPeerIds();
    CHECK(ids.size() == 3);

    // IDs should all be present (order not guaranteed)
    std::sort(ids.begin(), ids.end());
    CHECK(ids[0] == 1);
    CHECK(ids[1] == 2);
    CHECK(ids[2] == 3);
}

TEST_CASE("PeerManager failure count reset on connect") {
    PeerManager pm;
    pm.AddPeer(1, "127.0.0.1:9000");

    pm.RecordFailure(1);
    pm.RecordFailure(1);
    pm.RecordFailure(1);
    CHECK(pm.GetPeer(1)->failure_count == 3);

    pm.UpdateState(1, PeerState::Connected);
    CHECK(pm.GetPeer(1)->failure_count == 0);
}

TEST_CASE("RpcError ToString") {
    auto err = RpcError::ConnectionFailed("timeout");
    CHECK(err.ToString() == "ConnectionFailed: timeout");

    auto err2 = RpcError::InvalidMagic();
    CHECK(err2.ToString() == "InvalidMagic");
}

}  // TEST_SUITE("rpc")
