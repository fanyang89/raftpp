#pragma once

#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>

#include "raftpp/core/capnp_util.h"
#include "raftpp/core/types.h"

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
    auto msg = capnp_util::make<msg::RaftorMetadataChange>();
    auto builder = capnp_util::builder<msg::RaftorMetadataChange>(msg);

    switch (change.type) {
        case MetadataChangeType::UpsertPeerAddress: {
            auto peer = builder.initUpsertPeerAddress();
            peer.setNodeId(change.node_id);
            peer.setAddr(change.addr);
            break;
        }
    }

    return capnp_util::toString(msg);
}

inline std::optional<MetadataChange> ParseMetadataChange(std::string_view data) {
    try {
        auto msg = capnp_util::fromString<msg::RaftorMetadataChange>(data);
        auto reader = capnp_util::reader<msg::RaftorMetadataChange>(msg);

        switch (reader.which()) {
            case msg::RaftorMetadataChange::UPSERT_PEER_ADDRESS: {
                auto peer = reader.getUpsertPeerAddress();
                auto addr = peer.getAddr();
                MetadataChange change;
                change.type = MetadataChangeType::UpsertPeerAddress;
                change.node_id = peer.getNodeId();
                change.addr.assign(addr.cStr(), addr.size());
                return change;
            }
        }
    } catch (...) {
        return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace raftpp::raftor
