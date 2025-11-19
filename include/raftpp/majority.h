#pragma once

#include <functional>

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include "raftpp/primitives.h"
#include "raftpp/quorum.h"

namespace raftpp {

class MajorityConfig : public Set<uint64_t> {
  public:
    MajorityConfig();
    explicit MajorityConfig(const Set<uint64_t>& voters);

    [[nodiscard]] std::pair<uint64_t, bool> CommittedIndex(bool use_group_commit, const AckedIndexer& l) const;
    [[nodiscard]] VoteResult GetVoteResult(const std::function<std::optional<bool>(uint64_t)>& check) const;
};

void to_json(nlohmann::json& j, const MajorityConfig& p);
void from_json(const nlohmann::json& j, MajorityConfig& p);

}  // namespace raftpp

template <>
struct fmt::formatter<raftpp::MajorityConfig> : formatter<std::string_view> {
    static format_context::iterator format(const raftpp::MajorityConfig& value, const format_context& ctx);
};
