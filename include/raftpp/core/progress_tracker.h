#pragma once

#include "progress.h"
#include "tracker_conf.h"

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

    [[nodiscard]] VoteResult GetVoteResult(const Map<uint64_t, bool>& votes) const;
    [[nodiscard]] CountVoteResult CountVotes();
    void ApplyConf(const TrackerConfiguration& conf, const MapChange& changes, uint64_t next_idx);
    void ResetVotes();
    [[nodiscard]] std::pair<uint64_t, bool> MaxCommittedIndex() const;
    void RecordVote(uint64_t id, bool vote);
    [[nodiscard]] bool HasQuorum(const Set<uint64_t>& potential_quorum) const;
    [[nodiscard]] bool QuorumRecentlyActive(uint64_t perspective_of);
    [[nodiscard]] bool IsSingleton() const;

    [[nodiscard]] Progress* get(uint64_t id);
    [[nodiscard]] Progress& at(uint64_t id);
    [[nodiscard]] const Progress& at(uint64_t id) const;
    [[nodiscard]] TrackerConfiguration& conf();
    [[nodiscard]] const TrackerConfiguration& conf() const;
    [[nodiscard]] const ProgressMap& progress_map() const;
    [[nodiscard]] ProgressMap& progress_map();

    [[nodiscard]] const Map<uint64_t, bool>& votes() const { return votes_; }

    void EnableGroupCommit(bool enable);
    [[nodiscard]] bool GroupCommit() const;

  private:
    ProgressMap progress_;
    TrackerConfiguration conf_;
    Map<uint64_t, bool> votes_;
    size_t max_inflight_;
    bool group_commit_;
};

}  // namespace raftpp
