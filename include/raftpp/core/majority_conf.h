#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>

#include "ack_indexer.h"
#include "primitives.h"
#include "raftpp/fmt.h"

namespace raftpp {

class MajorityConfig : public Set<uint64_t> {
  public:
    MajorityConfig();
    explicit MajorityConfig(const Set<uint64_t>& voters);

    [[nodiscard]] std::pair<uint64_t, bool> CommittedIndex(
        bool use_group_commit, const AckedIndexer& l
    ) const;
    [[nodiscard]] VoteResult GetVoteResult(
        const std::function<std::optional<bool>(uint64_t)>& check
    ) const;
};

}  // namespace raftpp

template <>
struct fmt::formatter<raftpp::MajorityConfig> : formatter<std::string_view> {
    static format_context::iterator format(
        const raftpp::MajorityConfig& value, const format_context& ctx
    );
};
