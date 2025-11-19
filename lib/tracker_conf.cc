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

}  // namespace raftpp
