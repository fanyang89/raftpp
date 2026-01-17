#pragma once

#include <span>
#include <vector>

#include "raftpp/core/error.h"
#include "raftpp/core/raftpp.pb.h"

namespace raftpp::raftor::rpc {

/// Codec for message framing over TCP
///
/// Wire format:
/// ```
/// ┌─────────────────────────────────────────────────────────────────────────────┐
/// │  Magic (4 bytes)  │  Header Length (4 bytes)  │  RpcHeader  │  Payload      │
/// │    0x52415046     │     uint32_t LE           │  protobuf   │  protobuf     │
/// └─────────────────────────────────────────────────────────────────────────────┘
/// ```
class Codec {
  public:
    /// Magic number "RAPF" (RAftPP Frame)
    static constexpr uint32_t kMagic = 0x52415046;

    /// Fixed prefix size: magic (4) + header_length (4)
    static constexpr size_t kPrefixSize = 8;

    /// Current protocol version
    static constexpr uint32_t kVersion = 1;

    /// Default maximum message size (64 MB)
    static constexpr size_t kDefaultMaxMessageSize = 64 * 1024 * 1024;

    /// Encode a message to a buffer with frame header
    /// @param msg The message to encode
    /// @param from_node Source node ID
    /// @param to_node Destination node ID
    /// @param request_id Optional request ID for correlation
    static std::vector<uint8_t> Encode(
        const Message& msg, uint64_t from_node = 0, uint64_t to_node = 0, uint64_t request_id = 0
    );

    /// Decode result containing header and message
    struct DecodeResult {
        RpcHeader header;
        Message message;
        size_t bytes_consumed;
    };

    /// Decode a message from buffer
    ///
    /// Returns DecodeResult on success.
    /// If the buffer is incomplete (not enough data), returns result with bytes_consumed = 0.
    /// Returns error if the message is invalid or too large.
    static Result<DecodeResult> Decode(
        std::span<const uint8_t> buffer, size_t max_size = kDefaultMaxMessageSize
    );

    /// Check if buffer has a complete frame
    /// Returns the total frame size if complete, 0 if incomplete, or error
    static Result<size_t> FrameSize(
        std::span<const uint8_t> buffer, size_t max_size = kDefaultMaxMessageSize
    );

    /// Legacy header size for backward compatibility references
    static constexpr size_t kHeaderSize = kPrefixSize;
};

/// Handshake codec for connection establishment
///
/// Wire format:
/// ```
/// ┌─────────────────────────────────────────────────────────────────┐
/// │  Magic (4 bytes)  │  Length (4 bytes)  │  RpcHandshake          │
/// │    0x52415048     │   uint32_t LE      │  protobuf              │
/// └─────────────────────────────────────────────────────────────────┘
/// ```
class HandshakeCodec {
  public:
    /// Magic number "RAPH" (RAftPP Handshake)
    static constexpr uint32_t kMagic = 0x52415048;

    /// Fixed prefix size: magic (4) + length (4)
    static constexpr size_t kPrefixSize = 8;

    /// Current protocol version
    static constexpr uint32_t kVersion = 1;

    /// Encode a handshake message
    static std::vector<uint8_t> Encode(const RpcHandshake& hs);

    /// Decode a handshake message
    /// Returns (handshake, bytes_consumed) on success
    /// If buffer is incomplete, returns result with bytes_consumed = 0
    static Result<std::pair<RpcHandshake, size_t>> Decode(std::span<const uint8_t> buffer);
};

/// Legacy Handshake struct for backward compatibility
/// @deprecated Use HandshakeCodec with RpcHandshake instead
struct Handshake {
    static constexpr uint32_t kMagic = HandshakeCodec::kMagic;
    static constexpr uint16_t kVersion = 1;
    static constexpr size_t kSize = HandshakeCodec::kPrefixSize;  // Minimum size

    uint64_t node_id;
    uint64_t cluster_id = 0;

    std::vector<uint8_t> Encode() const;
    static Result<Handshake> Decode(std::span<const uint8_t> buffer);
};

/// Parse address string "host:port" into components
/// @param addr Address string in the format "host:port"
/// @return Pair of (host, port) on success, or RaftError on failure
[[nodiscard]] Result<std::pair<std::string, int>> ParseAddress(const std::string& addr);

}  // namespace raftpp::raftor::rpc
