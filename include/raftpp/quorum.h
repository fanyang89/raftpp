#pragma once

#include <optional>

#include "raftpp/primitives.h"

namespace raftpp {

enum class VoteResult : int {
    Pending,
    Lost,
    Won,
};

struct Index {
    uint64_t index;
    uint64_t group_id;
};

class AckedIndexer {
public:
    virtual ~AckedIndexer() = default;
    virtual std::optional<Index> AckedIndex(uint64_t voter) = 0;
};

class AckIndexer final : public AckedIndexer, Map<uint64_t, Index> {
public:
    std::optional<Index> AckedIndex(uint64_t voter) override;
};

}
