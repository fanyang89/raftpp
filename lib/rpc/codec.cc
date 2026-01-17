#include "raftpp/rpc/codec.h"

#include <cstring>

namespace raftpp::rpc {

using raftpp::RaftError;
using raftpp::Result;
using raftpp::RpcErrorCode;

std::vector<uint8_t> Codec::Encode(
    const Message& msg, uint64_t from_node, uint64_t to_node, uint64_t request_id
) {
    // Build RpcHeader
    RpcHeader header;
    header.set_version(kVersion);
    header.set_from_node(from_node);
    header.set_to_node(to_node);
    header.set_request_id(request_id);
    header.set_compression(COMPRESSION_NONE);
    header.set_payload_size(static_cast<uint32_t>(msg.ByteSizeLong()));
    header.set_msg_type(msg.msg_type());

    size_t header_size = header.ByteSizeLong();
    size_t payload_size = msg.ByteSizeLong();
    size_t total_size = kPrefixSize + header_size + payload_size;

    std::vector<uint8_t> buffer(total_size);

    // Write magic (little-endian)
    uint32_t magic = kMagic;
    std::memcpy(buffer.data(), &magic, sizeof(magic));

    // Write header length (little-endian)
    uint32_t header_len = static_cast<uint32_t>(header_size);
    std::memcpy(buffer.data() + 4, &header_len, sizeof(header_len));

    // Write RpcHeader
    header.SerializeToArray(buffer.data() + kPrefixSize, static_cast<int>(header_size));

    // Write payload
    msg.SerializeToArray(buffer.data() + kPrefixSize + header_size, static_cast<int>(payload_size));

    return buffer;
}

Result<size_t> Codec::FrameSize(std::span<const uint8_t> buffer, size_t max_size) {
    if (buffer.size() < kPrefixSize) {
        return 0;  // Incomplete prefix
    }

    // Read magic
    uint32_t magic;
    std::memcpy(&magic, buffer.data(), sizeof(magic));
    if (magic != kMagic) {
        return RaftError(RpcErrorCode::InvalidMagic);
    }

    // Read header length
    uint32_t header_len;
    std::memcpy(&header_len, buffer.data() + 4, sizeof(header_len));

    // Need full header to get payload size
    size_t min_frame_size = kPrefixSize + header_len;
    if (buffer.size() < min_frame_size) {
        return 0;  // Incomplete header
    }

    // Parse header to get payload size
    RpcHeader header;
    if (!header.ParseFromArray(buffer.data() + kPrefixSize, static_cast<int>(header_len))) {
        return RaftError(RpcErrorCode::HeaderParseFailed);
    }

    size_t total_size = kPrefixSize + header_len + header.payload_size();
    if (total_size > max_size) {
        return RaftError(RpcErrorCode::MessageTooLarge);
    }

    return total_size;
}

Result<Codec::DecodeResult> Codec::Decode(std::span<const uint8_t> buffer, size_t max_size) {
    if (buffer.size() < kPrefixSize) {
        return DecodeResult{{}, {}, 0};  // Incomplete
    }

    // Read magic
    uint32_t magic;
    std::memcpy(&magic, buffer.data(), sizeof(magic));
    if (magic != kMagic) {
        return RaftError(RpcErrorCode::InvalidMagic);
    }

    // Read header length
    uint32_t header_len;
    std::memcpy(&header_len, buffer.data() + 4, sizeof(header_len));

    size_t header_end = kPrefixSize + header_len;
    if (buffer.size() < header_end) {
        return DecodeResult{{}, {}, 0};  // Incomplete header
    }

    // Parse RpcHeader
    RpcHeader header;
    if (!header.ParseFromArray(buffer.data() + kPrefixSize, static_cast<int>(header_len))) {
        return RaftError(RpcErrorCode::HeaderParseFailed);
    }

    size_t total_size = header_end + header.payload_size();
    if (total_size > max_size) {
        return RaftError(RpcErrorCode::MessageTooLarge);
    }

    if (buffer.size() < total_size) {
        return DecodeResult{{}, {}, 0};  // Incomplete payload
    }

    // Parse Message payload
    Message msg;
    if (!msg.ParseFromArray(buffer.data() + header_end, static_cast<int>(header.payload_size()))) {
        return RaftError(RpcErrorCode::PayloadParseFailed);
    }

    return DecodeResult{std::move(header), std::move(msg), total_size};
}

// HandshakeCodec implementation
std::vector<uint8_t> HandshakeCodec::Encode(const RpcHandshake& hs) {
    size_t payload_size = hs.ByteSizeLong();
    std::vector<uint8_t> buffer(kPrefixSize + payload_size);

    // Write magic
    uint32_t magic = kMagic;
    std::memcpy(buffer.data(), &magic, sizeof(magic));

    // Write length
    uint32_t length = static_cast<uint32_t>(payload_size);
    std::memcpy(buffer.data() + 4, &length, sizeof(length));

    // Write payload
    hs.SerializeToArray(buffer.data() + kPrefixSize, static_cast<int>(payload_size));

    return buffer;
}

Result<std::pair<RpcHandshake, size_t>> HandshakeCodec::Decode(std::span<const uint8_t> buffer) {
    if (buffer.size() < kPrefixSize) {
        return std::pair<RpcHandshake, size_t>{{}, 0};  // Incomplete
    }

    // Read magic
    uint32_t magic;
    std::memcpy(&magic, buffer.data(), sizeof(magic));
    if (magic != kMagic) {
        return RaftError(RpcErrorCode::InvalidMagic);
    }

    // Read length
    uint32_t length;
    std::memcpy(&length, buffer.data() + 4, sizeof(length));

    size_t total_size = kPrefixSize + length;
    if (buffer.size() < total_size) {
        return std::pair<RpcHandshake, size_t>{{}, 0};  // Incomplete
    }

    // Parse RpcHandshake
    RpcHandshake hs;
    if (!hs.ParseFromArray(buffer.data() + kPrefixSize, static_cast<int>(length))) {
        return RaftError(RpcErrorCode::HandshakeParseFailed);
    }

    return std::pair{std::move(hs), total_size};
}

// Legacy Handshake implementation (delegates to HandshakeCodec)
std::vector<uint8_t> Handshake::Encode() const {
    RpcHandshake hs;
    hs.set_version(kVersion);
    hs.set_node_id(node_id);
    hs.set_cluster_id(cluster_id);
    return HandshakeCodec::Encode(hs);
}

Result<Handshake> Handshake::Decode(std::span<const uint8_t> buffer) {
    auto result = HandshakeCodec::Decode(buffer);
    if (!result) {
        return std::unexpected(result.error());
    }

    auto& [hs, consumed] = *result;
    if (consumed == 0) {
        return RaftError(RpcErrorCode::HandshakeBufferTooSmall);
    }

    Handshake legacy;
    legacy.node_id = hs.node_id();
    legacy.cluster_id = hs.cluster_id();
    return legacy;
}

}  // namespace raftpp::rpc
