#include "raftpp/raftor/rpc/codec.h"

#include <charconv>
#include <cstring>

#include <capnp/message.h>
#include <capnp/serialize.h>

namespace raftpp::raftor::rpc {

std::vector<uint8_t> Codec::Encode(
    const Message& msg, uint64_t from_node, uint64_t to_node, uint64_t request_id
) {
    // Build RpcHeader
    auto header = capnp_util::make<capnp::RpcHeader>();
    auto header_builder = capnp_util::builder<capnp::RpcHeader>(header);
    header_builder.setVersion(kVersion);
    header_builder.setFromNode(from_node);
    header_builder.setToNode(to_node);
    header_builder.setRequestId(request_id);
    header_builder.setCompression(static_cast<::raftpp::capnp::CompressionType>(
        static_cast<int>(capnp::CompressionType::COMPRESSION_NONE)
    ));

    // Serialize message to get payload size
    auto msg_bytes = capnp_util::toBytes(msg);
    header_builder.setPayloadSize(static_cast<uint32_t>(msg_bytes.size()));
    header_builder.setMsgType(static_cast<::raftpp::capnp::MessageType>(
        static_cast<int>(capnp_util::reader<msg::Message>(msg).getMsgType())
    ));

    // Serialize header
    auto header_bytes = capnp_util::toBytes(header);
    size_t header_size = header_bytes.size();
    size_t payload_size = msg_bytes.size();
    size_t total_size = kPrefixSize + header_size + payload_size;

    std::vector<uint8_t> buffer(total_size);

    // Write magic (little-endian)
    uint32_t magic = kMagic;
    std::memcpy(buffer.data(), &magic, sizeof(magic));

    // Write header length (little-endian)
    uint32_t header_len = static_cast<uint32_t>(header_size);
    std::memcpy(buffer.data() + 4, &header_len, sizeof(header_len));

    // Write RpcHeader
    std::memcpy(buffer.data() + kPrefixSize, header_bytes.data(), header_size);

    // Write payload
    std::memcpy(buffer.data() + kPrefixSize + header_size, msg_bytes.data(), payload_size);

    return buffer;
}

size_t Codec::FrameOverhead() {
    static const size_t overhead = []() {
        auto header = capnp_util::make<capnp::RpcHeader>();
        auto header_builder = capnp_util::builder<capnp::RpcHeader>(header);
        header_builder.setVersion(kVersion);
        header_builder.setFromNode(0);
        header_builder.setToNode(0);
        header_builder.setRequestId(0);
        header_builder.setCompression(static_cast<::raftpp::capnp::CompressionType>(
            static_cast<int>(capnp::CompressionType::COMPRESSION_NONE)
        ));
        header_builder.setPayloadSize(0);
        header_builder.setMsgType(
            static_cast<::raftpp::capnp::MessageType>(static_cast<int>(capnp::MessageType::MSG_HUP))
        );
        auto header_bytes = capnp_util::toBytes(header);
        return kPrefixSize + header_bytes.size();
    }();
    return overhead;
}

size_t Codec::MessageOverhead() {
    static const size_t overhead = []() {
        auto msg = capnp_util::make<msg::Message>();
        auto builder = capnp_util::builder<msg::Message>(msg);
        builder.setMsgType(
            static_cast<::raftpp::capnp::MessageType>(static_cast<int>(MessageType::MSG_HUP))
        );
        builder.initEntries(0);
        auto msg_bytes = capnp_util::toBytes(msg);
        return msg_bytes.size();
    }();
    return overhead;
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
    try {
        // Convert bytes to words for Cap'n Proto
        const ::capnp::word* words =
            reinterpret_cast<const ::capnp::word*>(buffer.data() + kPrefixSize);
        size_t word_count = header_len / sizeof(::capnp::word);

        ::capnp::FlatArrayMessageReader reader(kj::ArrayPtr<const ::capnp::word>(words, word_count)
        );
        auto header_reader = reader.getRoot<capnp::RpcHeader>();

        size_t total_size = kPrefixSize + header_len + header_reader.getPayloadSize();
        if (total_size > max_size) {
            return RaftError(RpcErrorCode::MessageTooLarge);
        }

        return total_size;
    } catch (...) {
        return RaftError(RpcErrorCode::HeaderParseFailed);
    }
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
    try {
        const ::capnp::word* words =
            reinterpret_cast<const ::capnp::word*>(buffer.data() + kPrefixSize);
        size_t word_count = header_len / sizeof(::capnp::word);
        header = capnp_util::fromWords<capnp::RpcHeader>(
            kj::ArrayPtr<const ::capnp::word>(words, word_count)
        );
    } catch (...) {
        return RaftError(RpcErrorCode::HeaderParseFailed);
    }

    auto header_reader = capnp_util::reader<capnp::RpcHeader>(header);
    size_t total_size = header_end + header_reader.getPayloadSize();
    if (total_size > max_size) {
        return RaftError(RpcErrorCode::MessageTooLarge);
    }

    if (buffer.size() < total_size) {
        return DecodeResult{{}, {}, 0};  // Incomplete payload
    }

    // Parse Message payload
    Message msg;
    try {
        const ::capnp::word* words =
            reinterpret_cast<const ::capnp::word*>(buffer.data() + header_end);
        size_t word_count = header_reader.getPayloadSize() / sizeof(::capnp::word);
        msg =
            capnp_util::fromWords<msg::Message>(kj::ArrayPtr<const ::capnp::word>(words, word_count)
            );
    } catch (...) {
        return RaftError(RpcErrorCode::PayloadParseFailed);
    }

    return DecodeResult{std::move(header), std::move(msg), total_size};
}

// HandshakeCodec implementation
std::vector<uint8_t> HandshakeCodec::Encode(const RpcHandshake& hs) {
    auto payload_bytes = capnp_util::toBytes(hs);
    std::vector<uint8_t> buffer(kPrefixSize + payload_bytes.size());

    // Write magic
    uint32_t magic = kMagic;
    std::memcpy(buffer.data(), &magic, sizeof(magic));

    // Write length
    uint32_t length = static_cast<uint32_t>(payload_bytes.size());
    std::memcpy(buffer.data() + 4, &length, sizeof(length));

    // Write payload
    std::memcpy(buffer.data() + kPrefixSize, payload_bytes.data(), payload_bytes.size());

    return buffer;
}

Result<std::pair<RpcHandshake, size_t>> HandshakeCodec::Decode(std::span<const uint8_t> buffer) {
    if (buffer.size() < kPrefixSize) {
        return std::make_pair(RpcHandshake{}, 0);  // Incomplete
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
        return std::make_pair(RpcHandshake{}, 0);  // Incomplete
    }

    // Parse RpcHandshake
    RpcHandshake hs;
    try {
        const ::capnp::word* words =
            reinterpret_cast<const ::capnp::word*>(buffer.data() + kPrefixSize);
        size_t word_count = length / sizeof(::capnp::word);
        hs = capnp_util::fromWords<capnp::RpcHandshake>(
            kj::ArrayPtr<const ::capnp::word>(words, word_count)
        );
    } catch (...) {
        return RaftError(RpcErrorCode::HandshakeParseFailed);
    }

    return std::make_pair(std::move(hs), total_size);
}

Result<std::pair<std::string, int>> ParseAddress(const std::string& addr) {
    auto colon = addr.rfind(':');
    if (colon == std::string::npos) {
        return RaftError(RpcErrorCode::AddressPortMissing);
    }

    std::string host = addr.substr(0, colon);
    std::string_view port_str = std::string_view(addr).substr(colon + 1);

    int port = 0;
    auto [ptr, ec] = std::from_chars(port_str.data(), port_str.data() + port_str.size(), port);
    if (ec != std::errc{} || ptr != port_str.data() + port_str.size()) {
        return RaftError(RpcErrorCode::AddressPortInvalid);
    }

    if (port <= 0 || port > 65535) {
        return RaftError(RpcErrorCode::AddressPortOutOfRange);
    }

    return std::make_pair(host, port);
}

}  // namespace raftpp::raftor::rpc
