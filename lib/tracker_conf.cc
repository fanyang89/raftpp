#include "raftpp/tracker_conf.h"

namespace raftpp {

TrackerConfiguration::TrackerConfiguration() = default;

TrackerConfiguration::TrackerConfiguration(const Set<uint64_t>& voters, const Set<uint64_t>& learners)
    : voters(voters), auto_leave(false) {}

void TrackerConfiguration::Clear() {
    voters.Clear();
    learners.clear();
    learners_next.clear();
    auto_leave = false;
}

ConfState TrackerConfiguration::ToConfState() {
    ConfState cs;

    for (const auto v : voters.incoming()) {
        cs.mutable_voters()->Add(v);
    }

    for (const auto v : voters.outgoing()) {
        cs.mutable_voters_outgoing()->Add(v);
    }

    for (const auto v : learners) {
        cs.mutable_learners()->Add(v);
    }

    for (const auto v : learners_next) {
        cs.mutable_learners_next()->Add(v);
    }

    cs.set_auto_leave(auto_leave);
    return cs;
}

}  // namespace raftpp
