#include "raftpp/core/ack_indexer.h"

#include <unordered_map>
#include <utility>

namespace raftpp {

std::optional<Index> AckIndexer::AckedIndex(const uint64_t voter) const {
    if (const auto it = find(voter); it != end()) {
        return it->second;
    }
    return std::nullopt;
}

}  // namespace raftpp
