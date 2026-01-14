#include "raftpp/conf_restore.h"

#include "raftpp/conf_changer.h"

namespace raftpp {

std::pair<std::vector<ConfChangeSingle>, std::vector<ConfChangeSingle>> ToConfChangeSingle(const ConfState& cs) {
    std::vector<ConfChangeSingle> outgoing;
    std::vector<ConfChangeSingle> incoming;

    for (const uint64_t id : cs.voters_outgoing()) {
        ConfChangeSingle s;
        s.set_node_id(id);
        s.set_change_type(AddNode);
        outgoing.emplace_back(s);
    }

    for (const uint64_t id : cs.voters_outgoing()) {
        ConfChangeSingle s;
        s.set_node_id(id);
        s.set_change_type(RemoveNode);
        incoming.emplace_back(s);
    }

    for (const uint64_t id : cs.voters()) {
        ConfChangeSingle s;
        s.set_node_id(id);
        s.set_change_type(AddNode);
        incoming.emplace_back(s);
    }

    for (const uint64_t id : cs.learners()) {
        ConfChangeSingle s;
        s.set_node_id(id);
        s.set_change_type(AddLearnerNode);
        incoming.emplace_back(s);
    }

    for (const uint64_t id : cs.learners_next()) {
        ConfChangeSingle s;
        s.set_node_id(id);
        s.set_change_type(AddLearnerNode);
        incoming.emplace_back(s);
    }

    return std::make_pair(outgoing, incoming);
}

Result<void> Restore(ProgressTracker& tracker, uint64_t next_idx, const ConfState& cs) {
    const auto& [outgoing, incoming] = ToConfChangeSingle(cs);

    if (outgoing.empty()) {
        // When restoring an initial configuration, use EnterJoint to add all voters at once.
        // This avoids the "more than one voter changed" error that occurs when adding
        // multiple voters individually through Simple().
        auto changer = ConfChanger(tracker);
        auto result = changer.EnterJoint(false, incoming);
        if (!result) {
            // If EnterJoint fails (e.g., already in a joint config), fall back to Simple
            // but process all voters in a single batch to avoid the diff check
            auto p = changer.CheckAndCopy();
            if (!p) {
                return p.error();
            }

            // Apply all voter additions to the copy
            TrackerConfiguration& cfg = p->first;
            IncrChangeMap& prs = p->second;

            for (const ConfChangeSingle& i : incoming) {
                if (const auto r = changer.Apply(cfg, prs, std::span{&i, 1}); !r) {
                    return r.error();
                }
            }

            // Now check invariants on the final configuration
            if (const auto r = CheckInvariants(cfg, prs); !r) {
                return r.error();
            }

            // Apply the changes
            tracker.ApplyConf(cfg, prs.ToChanges(), next_idx);
        } else {
            tracker.ApplyConf(result->first, result->second, next_idx);
        }
    } else {
        for (const ConfChangeSingle& cc : outgoing) {
            if (const auto r = ConfChanger(tracker).Simple(cc)) {
                const TrackerConfiguration& cfg = r->first;
                const MapChange& changes = r->second;
                tracker.ApplyConf(cfg, changes, next_idx);
            } else {
                return r.error();
            }
        }

        if (const auto r = ConfChanger(tracker).EnterJoint(cs.auto_leave(), incoming)) {
            const TrackerConfiguration& cfg = r->first;
            const MapChange& changes = r->second;
            tracker.ApplyConf(cfg, changes, next_idx);
        } else {
            return r.error();
        }
    }

    return {};
}

}  // namespace raftpp
