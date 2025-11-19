#pragma once

#include "raftpp/joint.h"
#include "raftpp/progress.h"

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

class ProgressTracker {
  public:
    explicit ProgressTracker(size_t max_inflight);

    struct CountVoteResult {
        size_t granted;
        size_t rejected;
        VoteResult result;
    };

    VoteResult GetVoteResult(const Map<uint64_t, bool>& votes) const;
    CountVoteResult CountVote();

    TrackerConfiguration& conf();

  private:
    ProgressMap progress_;
    TrackerConfiguration conf_;
    Map<uint64_t, bool> votes_;
    size_t max_inflight_;
    bool group_commit_;
};

}  // namespace raftpp
