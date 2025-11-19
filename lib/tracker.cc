#include "raftpp/tracker.h"

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

ProgressTracker::ProgressTracker(const size_t max_inflight) : max_inflight_(max_inflight), group_commit_(false) {}

VoteResult ProgressTracker::GetVoteResult(const Map<uint64_t, bool>& votes) const {
    return conf_.voters.GetVoteResult([&votes](const uint64_t id) -> bool { return votes.at(id); });
}

ProgressTracker::CountVoteResult ProgressTracker::CountVote() {
    size_t granted = 0;
    size_t rejected = 0;

    for (const auto& [id, vote] : votes_) {
        if (!conf_.voters.Contains(id)) {
            continue;
        }
        if (vote) {
            granted++;
        } else {
            rejected++;
        }
    }
    const auto r = GetVoteResult(votes_);
    return {granted, rejected, r};
}

TrackerConfiguration& ProgressTracker::conf() {
    return conf_;
}

}  // namespace raftpp
