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
    CountVoteResult CountVotes();
    void ApplyConf(const TrackerConfiguration& conf, const MapChange& changes, uint64_t next_idx);
    void ResetVotes();
    std::pair<uint64_t, bool> MaxCommittedIndex() const;
    void RecordVote(uint64_t id, bool vote);
    bool HasQuorum(const Set<uint64_t>& potential_quorum) const;
    bool QuorumRecentlyActive(uint64_t perspective_of);

    Progress* get(uint64_t id);
    Progress& at(uint64_t id);
    const Progress& at(uint64_t id) const;
    TrackerConfiguration& conf();
    const TrackerConfiguration& conf() const;
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
