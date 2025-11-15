#pragma once

#include "joint_conf.h"
#include "primitives.h"
#include "types.h"

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

}  // namespace raftpp

template <>
struct fmt::formatter<raftpp::TrackerConfiguration> : formatter<std::string_view> {
    static format_context::iterator format(
        const raftpp::TrackerConfiguration& value, const format_context& ctx
    );
};
