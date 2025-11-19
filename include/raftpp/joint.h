#pragma once

#include <spdlog/fmt/fmt.h>

#include "raftpp/majority.h"

namespace raftpp {

class JointConfiguration {
  public:
    JointConfiguration();
    explicit JointConfiguration(const Set<uint64_t>& voters);
    JointConfiguration(const Set<uint64_t>& incoming, const Set<uint64_t>& outgoing);

    std::pair<uint64_t, bool> CommittedIndex(bool use_group_commit, AckedIndexer& l) const;
    VoteResult GetVoteResult(const std::function<std::optional<bool>(uint64_t)>& check) const;
    void Clear();
    bool Contains(uint64_t id) const;
    Set<uint64_t> IDs() const;

    MajorityConfig& outgoing();
    const MajorityConfig& outgoing() const;
    MajorityConfig& incoming();
    const MajorityConfig& incoming() const;

  private:
    MajorityConfig incoming_;
    MajorityConfig outgoing_;
};

void to_json(nlohmann::json& j, const JointConfiguration& p);
void from_json(const nlohmann::json& j, JointConfiguration& p);

}  // namespace raftpp

template <>
struct fmt::formatter<raftpp::JointConfiguration> : formatter<std::string_view> {
    static format_context::iterator format(const raftpp::JointConfiguration& value, const format_context& ctx);
};
