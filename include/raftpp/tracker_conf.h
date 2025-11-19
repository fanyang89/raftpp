#pragma once

#include "raftpp/joint.h"
#include "raftpp/primitives.h"

namespace raftpp {

struct TrackerConfiguration {
    TrackerConfiguration();
    TrackerConfiguration(const Set<uint64_t>& voters, const Set<uint64_t>& learners);

    void Clear();

    JointConfiguration voters;
    Set<uint64_t> learners;
    Set<uint64_t> learners_next;
    bool auto_leave;
};

}  // namespace raftpp
