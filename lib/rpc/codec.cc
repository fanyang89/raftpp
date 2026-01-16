#include "raftpp/rpc/codec.h"

#include <cstring>

#include <spdlog/fmt/fmt.h>

namespace raftpp::rpc {

std::string RpcError::ToString() const {
    static constexpr const char* kCodeNames[] = {
        "ConnectionFailed", "ConnectionClosed", "MessageTooLarge", "InvalidMessage",
        "Timeout",          "InvalidMagic",     "InvalidAddress",
    };
    auto code_name = kCodeNames[static_cast<int>(code)];
    if (message.empty()) {
        return code_name;
    }
    return fmt::format("{}: {}", code_name, message);
}

std::vector<uint8_t> Codec::Encode(const Message& msg) {
    size_t payload_size = msg.ByteSizeLong();
    std::vector<uint8_t> buffer(kHeaderSize + payload_size);

    // Write magic (little-endian)
    uint32_t magic = kMagic;
    std::memcpy(buffer.data(), &magic, sizeof(magic));

    // Write length (little-endian)
    uint32_t length = static_cast<uint32_t>(payload_size);
    std::memcpy(buffer.data() + 4, &length, sizeof(length));

    // Write payload
    msg.SerializeToArray(buffer.data() + kHeaderSize, static_cast<int>(payload_size));

    return buffer;
}

RpcResult<size_t> Codec::FrameSize(std::span<const uint8_t> buffer, size_t max_size) {
    if (buffer.size() < kHeaderSize) {
        return 0;  // Incomplete header
    }

    // Read magic
    uint32_t magic;
    std::memcpy(&magic, buffer.data(), sizeof(magic));
    if (magic != kMagic) {
        return std::unexpected(
            RpcError::InvalidMagic(fmt::format("expected 0x{:08X}, got 0x{:08X}", kMagic, magic))
        );
    }

    // Read length
    uint32_t length;
    std::memcpy(&length, buffer.data() + 4, sizeof(length));

    if (length > max_size) {
        return std::unexpected(
            RpcError::MessageTooLarge(fmt::format("message size {} exceeds max {}", length, max_size)
            )
        );
    }

    return kHeaderSize + length;
}

RpcResult<std::pair<Message, size_t>> Codec::Decode(std::span<const uint8_t> buffer, size_t max_size
) {
    auto frame_size_result = FrameSize(buffer, max_size);
    if (!frame_size_result) {
        return std::unexpected(frame_size_result.error());
    }

    size_t frame_size = *frame_size_result;
    if (frame_size == 0 || buffer.size() < frame_size) {
        // Incomplete frame
        return std::pair<Message, size_t>{{}, 0};
    }

    // Parse protobuf payload
    Message msg;
    if (!msg.ParseFromArray(buffer.data() + kHeaderSize, static_cast<int>(frame_size - kHeaderSize))
    ) {
        return std::unexpected(RpcError::InvalidMessage("failed to parse protobuf message"));
    }

    return std::pair{std::move(msg), frame_size};
}

std::vector<uint8_t> Handshake::Encode() const {
    std::vector<uint8_t> buffer(kSize);

    // Write magic
    uint32_t magic = kMagic;
    std::memcpy(buffer.data(), &magic, sizeof(magic));

    // Write version
    uint16_t version = kVersion;
    std::memcpy(buffer.data() + 4, &version, sizeof(version));

    // Write node_id
    std::memcpy(buffer.data() + 6, &node_id, sizeof(node_id));

    return buffer;
}

RpcResult<Handshake> Handshake::Decode(std::span<const uint8_t> buffer) {
    if (buffer.size() < kSize) {
        return std::unexpected(RpcError::InvalidMessage("handshake buffer too small"));
    }

    // Read magic
    uint32_t magic;
    std::memcpy(&magic, buffer.data(), sizeof(magic));
    if (magic != kMagic) {
        return std::unexpected(
            RpcError::InvalidMagic(fmt::format("expected 0x{:08X}, got 0x{:08X}", kMagic, magic))
        );
    }

    // Read version
    uint16_t version;
    std::memcpy(&version, buffer.data() + 4, sizeof(version));
    if (version != kVersion) {
        return std::unexpected(
            RpcError::InvalidMessage(fmt::format("unsupported version {}", version))
        );
    }

    // Read node_id
    Handshake hs;
    std::memcpy(&hs.node_id, buffer.data() + 6, sizeof(hs.node_id));

    return hs;
}

}  // namespace raftpp::rpc
