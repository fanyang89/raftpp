#pragma once

#include "raftpp/joint_conf.h"
#include "raftpp/primitives.h"
#include "raftpp/raftpp.pb.h"

namespace raftpp {

struct TrackerConfiguration {
    TrackerConfiguration();
    TrackerConfiguration(const Set<uint64_t>& voters, const Set<uint64_t>& learners);

    void Clear();
    ConfState ToConfState();

    JointConfiguration voters;
    Set<uint64_t> learners;
    Set<uint64_t> learners_next;
    bool auto_leave;
};

void to_json(nlohmann::json& j, const TrackerConfiguration& p);
void from_json(const nlohmann::json& j, TrackerConfiguration& p);

}  // namespace raftpp

template <>
struct fmt::formatter<raftpp::TrackerConfiguration> : formatter<std::string_view> {
    static format_context::iterator format(const raftpp::TrackerConfiguration& value, const format_context& ctx);
};
