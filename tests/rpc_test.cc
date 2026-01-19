#include <doctest/doctest.h>
#include <kj/array.h>

#include "raftpp/raftor/rpc/codec.h"
#include "raftpp/raftor/rpc/peer_manager.h"

using namespace raftpp;
using namespace raftor::rpc;

namespace {

std::string DataToString(::capnp::Data::Reader data) {
    return std::string(reinterpret_cast<const char*>(data.begin()), data.size());
}

}  // namespace

TEST_SUITE("rpc") {
    TEST_CASE("Codec encode/decode round-trip") {
        Message msg;
        auto builder = msg.builder();
        builder.setMsgType(MessageType::MSG_APPEND);
        builder.setFrom(1);
        builder.setTo(2);
        builder.setTerm(5);
        builder.setIndex(100);
        builder.setCommit(50);

        auto encoded = Codec::Encode(msg, 1, 2, 42);

        // Check minimum size (prefix + header + payload)
        CHECK(encoded.size() >= Codec::kPrefixSize);

        auto result = Codec::Decode(encoded, Codec::kDefaultMaxMessageSize);
        REQUIRE(result.has_value());

        auto& decode_result = *result;
        CHECK(decode_result.bytes_consumed == encoded.size());
        auto msg_reader = msg.reader();
        auto decoded_reader = decode_result.message.reader();
        CHECK(decoded_reader.getMsgType() == msg_reader.getMsgType());
        CHECK(decoded_reader.getFrom() == msg_reader.getFrom());
        CHECK(decoded_reader.getTo() == msg_reader.getTo());
        CHECK(decoded_reader.getTerm() == msg_reader.getTerm());
        CHECK(decoded_reader.getIndex() == msg_reader.getIndex());
        CHECK(decoded_reader.getCommit() == msg_reader.getCommit());

        // Check RpcHeader fields
        auto header_reader = decode_result.header.reader();
        CHECK(header_reader.getVersion() == Codec::kVersion);
        CHECK(header_reader.getFromNode() == 1);
        CHECK(header_reader.getToNode() == 2);
        CHECK(header_reader.getRequestId() == 42);
        CHECK(header_reader.getMsgType() == MessageType::MSG_APPEND);
    }

    TEST_CASE("Codec handles incomplete buffer") {
        Message msg;
        auto builder = msg.builder();
        builder.setMsgType(MessageType::MSG_HEARTBEAT);
        builder.setFrom(1);
        builder.setTo(2);

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
        CHECK(result.error().Is(RpcErrorCode::InvalidMagic));
    }

    TEST_CASE("Codec rejects oversized message") {
        Message msg;
        auto builder = msg.builder();
        builder.setMsgType(MessageType::MSG_SNAPSHOT);
        builder.setFrom(1);
        builder.setTo(2);

        auto encoded = Codec::Encode(msg);

        // Try to decode with very small max size
        auto result = Codec::Decode(encoded, 1);  // 1 byte max
        REQUIRE(!result.has_value());
        CHECK(result.error().Is(RpcErrorCode::MessageTooLarge));
    }

    TEST_CASE("Codec handles message with entries") {
        Message msg;
        auto builder = msg.builder();
        builder.setMsgType(MessageType::MSG_APPEND);
        builder.setFrom(1);
        builder.setTo(2);
        builder.setTerm(3);

        // Add entries
        auto entries = builder.initEntries(10);
        for (int i = 0; i < 10; i++) {
            entries[i].setTerm(3);
            entries[i].setIndex(i + 1);
            auto data = std::string("test data ") + std::to_string(i);
            entries[i].setData(kj::arrayPtr(
                reinterpret_cast<const kj::byte*>(data.data()), data.size()));
        }

        auto encoded = Codec::Encode(msg);
        auto result = Codec::Decode(encoded, Codec::kDefaultMaxMessageSize);
        REQUIRE(result.has_value());

        auto& decode_result = *result;
        auto decoded_reader = decode_result.message.reader();
        CHECK(decoded_reader.getEntries().size() == 10);
        CHECK(DataToString(decoded_reader.getEntries()[5].getData()) == "test data 5");
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
        auto builder = hs.builder();
        builder.setVersion(1);
        builder.setNodeId(54321);
        builder.setClusterId(888);

        auto encoded = HandshakeCodec::Encode(hs);
        CHECK(encoded.size() >= HandshakeCodec::kPrefixSize);

        auto result = HandshakeCodec::Decode(encoded);
        REQUIRE(result.has_value());
        auto& [decoded, consumed] = *result;
        CHECK(consumed == encoded.size());
        auto decoded_reader = decoded.reader();
        CHECK(decoded_reader.getVersion() == 1);
        CHECK(decoded_reader.getNodeId() == 54321);
        CHECK(decoded_reader.getClusterId() == 888);
    }

    TEST_CASE("Handshake rejects invalid magic") {
        std::vector<uint8_t> bad_hs(HandshakeCodec::kPrefixSize, 0);

        auto result = Handshake::Decode(bad_hs);
        REQUIRE(!result.has_value());
        CHECK(result.error().Is(RpcErrorCode::InvalidMagic));
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

}  // TEST_SUITE("rpc")
