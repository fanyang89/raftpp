#include "raftpp/progress_tracker.h"

#include <libassert/assert.hpp>

namespace raftpp {

ProgressTracker::ProgressTracker(const size_t max_inflight) : max_inflight_(max_inflight), group_commit_(false) {}

VoteResult ProgressTracker::GetVoteResult(const Map<uint64_t, bool>& votes) const {
    return conf_.voters.GetVoteResult([&votes](const uint64_t id) -> bool { return votes.at(id); });
}

ProgressTracker::CountVoteResult ProgressTracker::CountVotes() {
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

std::pair<uint64_t, bool> ProgressTracker::MaxCommittedIndex() const {
    return conf_.voters.CommittedIndex(group_commit_, progress_);
}

void ProgressTracker::RecordVote(uint64_t id, bool vote) {
    votes_.emplace(id, vote);
}

bool ProgressTracker::HasQuorum(const Set<uint64_t>& potential_quorum) const {
    const auto checkFn = [&potential_quorum](const uint64_t id) -> bool {
        return potential_quorum.contains(id);
    };
    return conf_.voters.GetVoteResult(checkFn) == VoteResult::Won;
}

bool ProgressTracker::QuorumRecentlyActive(const uint64_t perspective_of) {
    Set<uint64_t> active;
    for (auto& [id, pr] : progress_) {
        if (id == perspective_of) {
            active.emplace(id);
            pr.recent_active() = true;
        } else if (pr.recent_active()) {
            active.emplace(id);
            pr.recent_active() = false;
        }
    }
    return HasQuorum(active);
}

Progress* ProgressTracker::get(const uint64_t id) {
    const auto it = progress_.find(id);
    if (it == progress_.end()) {
        return nullptr;
    }
    return &it->second;
}

Progress& ProgressTracker::at(const uint64_t id) {
    ASSERT(progress_.contains(id));
    return progress_.at(id);
}

const Progress& ProgressTracker::at(const uint64_t id) const {
    ASSERT(progress_.contains(id));
    return progress_.at(id);
}

TrackerConfiguration& ProgressTracker::conf() {
    return conf_;
}

const TrackerConfiguration& ProgressTracker::conf() const {
    return conf_;
}

const ProgressMap& ProgressTracker::progress_map() const {
    return progress_;
}

ProgressMap& ProgressTracker::progress_map() {
    return progress_;
}

}  // namespace raftpp
