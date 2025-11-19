#include "raftpp/tracker.h"

#include <libassert/assert.hpp>

namespace raftpp {

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

void ProgressTracker::ApplyConf(const TrackerConfiguration& conf, const MapChange& changes, const uint64_t next_idx) {
    conf_ = conf;

    for (const auto& change : changes) {
        const uint64_t id = change.first;
        const MapChangeType change_type = change.second;
        if (change_type == MapChangeType::Add) {
            Progress pr(next_idx, max_inflight_);
            pr.recent_active() = true;
            progress_.emplace(id, pr);
        } else if (change_type == MapChangeType::Remove) {
            progress_.erase(id);
        } else {
            PANIC("invalid change type");
        }
    }
}

void ProgressTracker::ResetVotes() {
    votes_.clear();
}

TrackerConfiguration& ProgressTracker::conf() {
    return conf_;
}

const TrackerConfiguration& ProgressTracker::conf() const {
    return conf_;
}

ProgressMap& ProgressTracker::progress() {
    return progress_;
}

const ProgressMap& ProgressTracker::progress() const {
    return progress_;
}

const ProgressMap& ProgressTracker::progress_map() const {
    return progress_;
}

ProgressMap& ProgressTracker::progress_map() {
    return progress_;
}

}  // namespace raftpp
