#pragma once

#include <stdint.h>

#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace raftpp::raftor {

constexpr std::string_view kMetadataProposalContextPrefix = "raftor:metadata:";

enum class MetadataChangeType : uint8_t {
    UpsertPeerAddress = 1,
};

struct MetadataChange {
    MetadataChangeType type = MetadataChangeType::UpsertPeerAddress;
    uint64_t node_id = 0;
    std::string addr;
};

inline bool IsMetadataProposalContext(std::string_view ctx) {
    return ctx.size() >= kMetadataProposalContextPrefix.size() &&
        ctx.substr(0, kMetadataProposalContextPrefix.size()) == kMetadataProposalContextPrefix;
}

inline std::string SerializeMetadataChange(const MetadataChange& change) {
    std::string data;
    data.reserve(32 + change.addr.size());
    data.append("RAFTOR_META_V1\n");
    data.push_back(static_cast<char>(change.type));
    data.append(reinterpret_cast<const char*>(&change.node_id), sizeof(change.node_id));
    uint32_t addr_size = static_cast<uint32_t>(change.addr.size());
    data.append(reinterpret_cast<const char*>(&addr_size), sizeof(addr_size));
    data.append(change.addr);
    return data;
}

inline std::optional<MetadataChange> ParseMetadataChange(std::string_view data) {
    constexpr std::string_view kMagic = "RAFTOR_META_V1\n";
    if (data.size() < kMagic.size() + 1 + sizeof(uint64_t) + sizeof(uint32_t) ||
        data.substr(0, kMagic.size()) != kMagic) {
        return std::nullopt;
    }

    size_t offset = kMagic.size();
    auto type = static_cast<MetadataChangeType>(static_cast<uint8_t>(data[offset]));
    ++offset;

    uint64_t node_id = 0;
    std::memcpy(&node_id, data.data() + offset, sizeof(node_id));
    offset += sizeof(node_id);

    uint32_t addr_size = 0;
    std::memcpy(&addr_size, data.data() + offset, sizeof(addr_size));
    offset += sizeof(addr_size);
    if (offset + addr_size != data.size()) {
        return std::nullopt;
    }

    if (type != MetadataChangeType::UpsertPeerAddress) {
        return std::nullopt;
    }

    MetadataChange change;
    change.type = type;
    change.node_id = node_id;
    change.addr.assign(data.data() + offset, addr_size);
    return change;
}

}  // namespace raftpp::raftor
