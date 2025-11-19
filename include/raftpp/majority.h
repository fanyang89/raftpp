#pragma once

#include <functional>

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include "raftpp/primitives.h"
#include "raftpp/quorum.h"

namespace raftpp {

class MajorityConfig {
  public:
    MajorityConfig();
    explicit MajorityConfig(const Set<uint64_t>& voters);

    [[nodiscard]] std::pair<uint64_t, bool> CommittedIndex(bool use_group_commit, AckedIndexer& l) const;
    [[nodiscard]] VoteResult GetVoteResult(const std::function<std::optional<bool>(uint64_t)>& check) const;
    void Clear();
    bool Contains(uint64_t id) const;

    Set<uint64_t>& mutable_voters();
    const Set<uint64_t>& voters() const;

  private:
    Set<uint64_t> voters_;
};

void to_json(nlohmann::json& j, const MajorityConfig& p);
void from_json(const nlohmann::json& j, MajorityConfig& p);

}  // namespace raftpp

template <>
struct fmt::formatter<raftpp::MajorityConfig> {
    format_context::iterator format(const raftpp::MajorityConfig& value, format_context& ctx) const;
};
