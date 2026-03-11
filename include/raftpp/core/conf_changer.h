#pragma once

#include "progress_tracker.h"
#include "raftpp/core/error.h"
#include "tracker_conf.h"
#include "types.h"

namespace raftpp {

class IncrChangeMap {
  public:
    explicit IncrChangeMap(ProgressMap& base);

    [[nodiscard]] bool Contains(uint64_t id) const;
    [[nodiscard]] MapChange ToChanges() const;

    [[nodiscard]] MapChange& changes();
    [[nodiscard]] const MapChange& changes() const;

  private:
    MapChange changes_;
    ProgressMap& base_;
};

class ConfChanger {
  public:
    explicit ConfChanger(ProgressTracker& tracker);

    [[nodiscard]] Result<std::pair<TrackerConfiguration, MapChange>>
    EnterJoint(bool auto_leave, std::span<const ConfChangeSingle>);

    [[nodiscard]] Result<
        std::pair<TrackerConfiguration, std::vector<std::pair<uint64_t, MapChangeType>>>, RaftError>
    LeaveJoint();

    [[nodiscard]] Result<std::pair<TrackerConfiguration, MapChange>> Simple(
        const ConfChangeSingle& ccs
    ) const;

    [[nodiscard]] Result<std::pair<TrackerConfiguration, MapChange>> Simple(
        std::span<const ConfChangeSingle> ccs
    ) const;

    [[nodiscard]] static Result<void> Apply(
        TrackerConfiguration& cfg, IncrChangeMap& prs, std::span<const ConfChangeSingle> ccs
    );

    [[nodiscard]] Result<std::pair<TrackerConfiguration, IncrChangeMap>> CheckAndCopy() const;

  private:
    static void MakeVoter(TrackerConfiguration& cfg, IncrChangeMap& prs, uint64_t id);
    static void MakeLearner(TrackerConfiguration& cfg, IncrChangeMap& prs, uint64_t id);
    static void Remove(TrackerConfiguration& cfg, IncrChangeMap& prs, uint64_t id);
    static void InitProgress(
        TrackerConfiguration& cfg, IncrChangeMap& prs, uint64_t id, bool is_learner
    );

    ProgressTracker& tracker_;
};

[[nodiscard]] bool Joint(const TrackerConfiguration& cfg);

[[nodiscard]] Result<void> CheckInvariants(
    const TrackerConfiguration& cfg, const IncrChangeMap& prs
);

}  // namespace raftpp
