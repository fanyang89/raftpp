#pragma once

#include "raftpp/error.h"
#include "raftpp/raftpp.pb.h"
#include "raftpp/tracker.h"
#include "raftpp/tracker_conf.h"

namespace raftpp {

class IncrChangeMap {
  public:
    explicit IncrChangeMap(ProgressMap& base);

    [[nodiscard]] bool Contains(uint64_t id) const;
    MapChange ToChanges() const;

    MapChange& changes();
    const MapChange& changes() const;

  private:
    MapChange changes_;
    ProgressMap& base_;
};

class Changer {
  public:
    explicit Changer(ProgressTracker& tracker);

    Result<std::pair<TrackerConfiguration, MapChange>> EnterJoint(bool auto_leave, std::span<const ConfChangeSingle>);
    Result<std::pair<TrackerConfiguration, MapChange>> Simple(const ConfChangeSingle& ccs) const;
    Result<std::pair<TrackerConfiguration, MapChange>> Simple(std::span<const ConfChangeSingle> ccs) const;

    static Result<void> Apply(TrackerConfiguration& cfg, IncrChangeMap& prs, std::span<const ConfChangeSingle> ccs);

  private:
    Result<std::pair<TrackerConfiguration, IncrChangeMap>> CheckAndCopy() const;

    static void MakeVoter(TrackerConfiguration& cfg, IncrChangeMap& prs, uint64_t id);
    static void MakeLearner(TrackerConfiguration& cfg, IncrChangeMap& prs, uint64_t id);
    static void Remove(TrackerConfiguration& cfg, IncrChangeMap& prs, uint64_t id);
    static void InitProgress(TrackerConfiguration& cfg, IncrChangeMap& prs, uint64_t id, bool is_learner);

    ProgressTracker& tracker_;
};

bool Joint(const TrackerConfiguration& cfg);

Result<void> CheckInvariants(const TrackerConfiguration& cfg, const IncrChangeMap& prs);

}  // namespace raftpp
