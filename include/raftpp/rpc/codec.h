#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "raftpp/raftpp.pb.h"

namespace raftpp::rpc {

/// Error codes for RPC operations
enum class RpcErrorCode {
    /// Connection to peer failed
    ConnectionFailed,
    /// Connection was closed unexpectedly
    ConnectionClosed,
    /// Message exceeds maximum allowed size
    MessageTooLarge,
    /// Message is malformed or invalid
    InvalidMessage,
    /// Operation timed out
    Timeout,
    /// Invalid magic number in frame header
    InvalidMagic,
    /// Address parsing failed
    InvalidAddress,
};

/// RPC error with code and optional message
struct RpcError {
    RpcErrorCode code;
    std::string message;

    static RpcError ConnectionFailed(std::string msg = "") {
        return {RpcErrorCode::ConnectionFailed, std::move(msg)};
    }
    static RpcError ConnectionClosed(std::string msg = "") {
        return {RpcErrorCode::ConnectionClosed, std::move(msg)};
    }
    static RpcError MessageTooLarge(std::string msg = "") {
        return {RpcErrorCode::MessageTooLarge, std::move(msg)};
    }
    static RpcError InvalidMessage(std::string msg = "") {
        return {RpcErrorCode::InvalidMessage, std::move(msg)};
    }
    static RpcError Timeout(std::string msg = "") { return {RpcErrorCode::Timeout, std::move(msg)}; }
    static RpcError InvalidMagic(std::string msg = "") {
        return {RpcErrorCode::InvalidMagic, std::move(msg)};
    }
    static RpcError InvalidAddress(std::string msg = "") {
        return {RpcErrorCode::InvalidAddress, std::move(msg)};
    }

    std::string ToString() const;
};

template <typename T>
using RpcResult = std::expected<T, RpcError>;

/// Codec for message framing over TCP
///
/// Wire format:
/// ```
/// ┌──────────────────────────────────────────────────────┐
/// │  Magic (4 bytes)  │  Length (4 bytes)  │  Payload   │
/// │    0x52415046     │    uint32_t LE     │  protobuf  │
/// └──────────────────────────────────────────────────────┘
/// ```
class Codec {
  public:
    /// Magic number "RAPF" (RAftPP Frame)
    static constexpr uint32_t kMagic = 0x52415046;

    /// Header size: magic (4) + length (4)
    static constexpr size_t kHeaderSize = 8;

    /// Default maximum message size (64 MB)
    static constexpr size_t kDefaultMaxMessageSize = 64 * 1024 * 1024;

    /// Encode a message to a buffer with frame header
    static std::vector<uint8_t> Encode(const Message& msg);

    /// Decode a message from buffer
    ///
    /// Returns (message, bytes_consumed) on success.
    /// If the buffer is incomplete (not enough data), returns (empty message, 0).
    /// Returns error if the message is invalid or too large.
    static RpcResult<std::pair<Message, size_t>> Decode(
        std::span<const uint8_t> buffer, size_t max_size = kDefaultMaxMessageSize
    );

    /// Check if buffer has a complete frame
    /// Returns the total frame size if complete, 0 if incomplete, or error
    static RpcResult<size_t> FrameSize(
        std::span<const uint8_t> buffer, size_t max_size = kDefaultMaxMessageSize
    );
};

/// Handshake message sent when establishing connection
///
/// Wire format:
/// ```
/// ┌──────────────────────────────────────────┐
/// │  Magic (4)  │  Version (2)  │ NodeID (8) │
/// └──────────────────────────────────────────┘
/// ```
struct Handshake {
    static constexpr uint32_t kMagic = 0x52415048;  // "RAPH" (RAftPP Handshake)
    static constexpr uint16_t kVersion = 1;
    static constexpr size_t kSize = 14;  // 4 + 2 + 8

    uint64_t node_id;

    std::vector<uint8_t> Encode() const;
    static RpcResult<Handshake> Decode(std::span<const uint8_t> buffer);
};

}  // namespace raftpp::rpc
