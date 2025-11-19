#pragma once

#include "raftpp/progress.h"
#include "raftpp/tracker_conf.h"

namespace raftpp {

enum class MapChangeType : uint8_t {
    Add,
    Remove,
};

using MapChange = std::vector<std::pair<uint64_t, MapChangeType>>;

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
    void ApplyConf(const TrackerConfiguration& conf, const MapChange& changes, uint64_t next_idx);
    void ResetVotes();

    TrackerConfiguration& conf();
    const TrackerConfiguration& conf() const;
    ProgressMap& progress();
    const ProgressMap& progress() const;
    const ProgressMap& progress_map() const;
    ProgressMap& progress_map();

  private:
    ProgressMap progress_;
    TrackerConfiguration conf_;
    Map<uint64_t, bool> votes_;
    size_t max_inflight_;
    bool group_commit_;
};

}  // namespace raftpp
