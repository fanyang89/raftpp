#include "raftpp/conf_changer.h"

#include "raftpp/raftpp.pb.h"

namespace raftpp {

Result<void> CheckInvariants(const TrackerConfiguration& cfg, const IncrChangeMap& prs) {
    for (const uint64_t id : cfg.voters.IDs()) {
        if (!prs.Contains(id)) {
            return RaftError(ConfChangeError(fmt::format("no progress for voter {}", id)));
        }
    }

    for (const uint64_t id : cfg.learners) {
        if (!prs.Contains(id)) {
            return RaftError(ConfChangeError(fmt::format("no progress for learner {}", id)));
        }

        if (cfg.voters.outgoing().contains(id)) {
            return RaftError(ConfChangeError(fmt::format("{} is in learners and outgoing voters", id)));
        }

        if (cfg.voters.incoming().contains(id)) {
            return RaftError(ConfChangeError(fmt::format("{} is in learners and incoming voters", id)));
        }
    }

    for (const uint64_t id : cfg.learners_next) {
        if (!prs.Contains(id)) {
            return RaftError(ConfChangeError(fmt::format("no progress for learner(next) {}", id)));
        }

        if (!cfg.voters.outgoing().contains(id)) {
            return RaftError(ConfChangeError(fmt::format("{} is in learners_next and outgoing voters", id)));
        }
    }

    if (!Joint(cfg)) {
        if (!cfg.learners_next.empty()) {
            return RaftError(ConfChangeError("learners_next must be empty when not joint"));
        }
        if (cfg.auto_leave) {
            return RaftError(ConfChangeError("auto_leave must be false when not joint"));
        }
    }

    return {};
}

IncrChangeMap::IncrChangeMap(ProgressMap& base) : base_(base) {}

bool IncrChangeMap::Contains(uint64_t id) const {
    const auto f = [&id](const auto& p) {
        return p.first == id;
    };
    if (const auto it = std::find_if(changes_.rbegin(), changes_.rend(), f); it != changes_.rend()) {
        switch (it->second) {
            case MapChangeType::Add:
                return true;
            case MapChangeType::Remove:
                return false;
        }
    }
    return base_.contains(id);
}

MapChange IncrChangeMap::ToChanges() const {
    return changes_;
}

MapChange& IncrChangeMap::changes() {
    return changes_;
}

const MapChange& IncrChangeMap::changes() const {
    return changes_;
}

bool Joint(const TrackerConfiguration& cfg) {
    return !cfg.voters.outgoing().empty();
}

ConfChanger::ConfChanger(ProgressTracker& tracker) : tracker_(tracker) {}

Result<std::pair<TrackerConfiguration, MapChange>> ConfChanger::EnterJoint(
    const bool auto_leave, const std::span<const ConfChangeSingle> ccs
) {
    if (Joint(tracker_.conf())) {
        return RaftError(ConfChangeError{"config is already joint"});
    }

    if (auto p = CheckAndCopy(); !p) {
        return p.error();
    } else {
        TrackerConfiguration& cfg = p->first;
        IncrChangeMap& prs = p->second;

        if (cfg.voters.incoming().empty()) {
            return RaftError(ConfChangeError("can't make a zero-voter config joint"));
        }

        cfg.voters.outgoing().insert(cfg.voters.incoming().begin(), cfg.voters.incoming().end());
        if (const auto r = Apply(cfg, prs, ccs); !r) {
            return r.error();
        }
        cfg.auto_leave = auto_leave;
        if (const auto r = CheckInvariants(cfg, prs); !r) {
            return r.error();
        }
        return std::make_pair(cfg, prs.ToChanges());
    }
}

Result<std::pair<TrackerConfiguration, std::vector<std::pair<uint64_t, MapChangeType>>>, RaftError>
ConfChanger::LeaveJoint() {
    if (!Joint(tracker_.conf())) {
        return RaftError(ConfChangeError("can't leave a non-joint config"));
    }

    if (auto p = CheckAndCopy(); !p) {
        return p.error();
    } else {
        TrackerConfiguration& cfg = p->first;
        IncrChangeMap& prs = p->second;

        if (cfg.voters.outgoing().empty()) {
            return RaftError(ConfChangeError(fmt::format("configuration is not joint: {}", cfg)));
        }

        cfg.learners.insert(cfg.learners_next.begin(), cfg.learners_next.end());
        cfg.learners_next.clear();

        for (const auto id : cfg.voters.outgoing()) {
            if (!cfg.voters.incoming().contains(id) && !cfg.learners.contains(id)) {
                prs.changes().emplace_back(id, MapChangeType::Remove);
            }
        }

        cfg.voters.outgoing().clear();
        cfg.auto_leave = false;
        if (const auto r = CheckInvariants(cfg, prs); !r) {
            return r.error();
        }
        return std::make_pair(cfg, prs.ToChanges());
    }
}

Result<void> ConfChanger::Apply(
    TrackerConfiguration& cfg, IncrChangeMap& prs, const std::span<const ConfChangeSingle> ccs
) {
    for (const auto& cc : ccs) {
        if (cc.node_id() == 0) {
            continue;
        }
        switch (cc.change_type()) {
            case AddNode:
                MakeVoter(cfg, prs, cc.node_id());
                break;
            case RemoveNode:
                Remove(cfg, prs, cc.node_id());
                break;
            case AddLearnerNode:
                MakeLearner(cfg, prs, cc.node_id());
                break;
            default:
                PANIC("invalid change type");
        }
    }

    if (cfg.voters.incoming().empty()) {
        return RaftError(ConfChangeError("remove all voters"));
    }
    return {};
}

Result<std::pair<TrackerConfiguration, MapChange>> ConfChanger::Simple(const ConfChangeSingle& ccs) const {
    std::vector<ConfChangeSingle> v;
    v.emplace_back(ccs);
    return Simple(std::span{v.begin(), v.end()});
}

Result<std::pair<TrackerConfiguration, MapChange>> ConfChanger::Simple(
    const std::span<const ConfChangeSingle> ccs
) const {
    if (Joint(tracker_.conf())) {
        return RaftError(ConfChangeError("can't apply simple config change in joint config"));
    }

    if (auto p = CheckAndCopy(); !p) {
        return p.error();
    } else {
        TrackerConfiguration& cfg = p->first;
        IncrChangeMap& prs = p->second;

        if (const auto r = Apply(cfg, prs, ccs); !r) {
            return r.error();
        }

        std::vector<uint64_t> diff;
        std::ranges::set_symmetric_difference(
            cfg.voters.incoming(), tracker_.conf().voters.incoming(), std::back_inserter(diff)
        );
        if (diff.size() > 1) {
            return RaftError(ConfChangeError("more than one voter changed without entering joint config"));
        }

        if (const auto r = CheckInvariants(cfg, prs); !r) {
            return r.error();
        }
        return std::make_pair(cfg, prs.ToChanges());
    }
}

Result<std::pair<TrackerConfiguration, IncrChangeMap>> ConfChanger::CheckAndCopy() const {
    IncrChangeMap prs(tracker_.progress_map());
    if (auto r = CheckInvariants(tracker_.conf(), prs); !r) {
        return r.error();
    }
    return std::make_pair(TrackerConfiguration(tracker_.conf()), prs);
}

void ConfChanger::InitProgress(TrackerConfiguration& cfg, IncrChangeMap& prs, uint64_t id, const bool is_learner) {
    if (!is_learner) {
        cfg.voters.incoming().insert(id);
    } else {
        cfg.learners.insert(id);
    }
    prs.changes().emplace_back(id, MapChangeType::Add);
}

void ConfChanger::MakeVoter(TrackerConfiguration& cfg, IncrChangeMap& prs, uint64_t id) {
    if (!prs.Contains(id)) {
        InitProgress(cfg, prs, id, false);
        return;
    }

    cfg.voters.incoming().insert(id);
    cfg.learners.erase(id);
    cfg.learners_next.erase(id);
}

void ConfChanger::MakeLearner(TrackerConfiguration& cfg, IncrChangeMap& prs, const uint64_t id) {
    if (!prs.Contains(id)) {
        InitProgress(cfg, prs, id, true);
        return;
    }

    if (cfg.learners.contains(id)) {
        return;
    }

    cfg.voters.incoming().erase(id);
    cfg.learners.erase(id);
    cfg.learners_next.erase(id);

    if (cfg.voters.outgoing().contains(id)) {
        cfg.learners_next.insert(id);
    } else {
        cfg.learners.insert(id);
    }
}

void ConfChanger::Remove(TrackerConfiguration& cfg, IncrChangeMap& prs, uint64_t id) {
    if (!prs.Contains(id)) {
        return;
    }

    cfg.voters.incoming().erase(id);
    cfg.learners.erase(id);
    cfg.learners_next.erase(id);

    // If the peer is still a voter in the outgoing config, keep the Progress.
    if (!cfg.voters.outgoing().contains(id)) {
        prs.changes().emplace_back(id, MapChangeType::Remove);
    }
}

}  // namespace raftpp
