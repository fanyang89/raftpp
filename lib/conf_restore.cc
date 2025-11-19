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
        for (const ConfChangeSingle& i : incoming) {
            if (const auto r = Changer(tracker).Simple(i)) {
                tracker.ApplyConf(r->first, r->second, next_idx);
            } else {
                return r.error();
            }
        }
    } else {
        for (const ConfChangeSingle& cc : outgoing) {
            if (const auto r = Changer(tracker).Simple(cc)) {
                const TrackerConfiguration& cfg = r->first;
                const MapChange& changes = r->second;
                tracker.ApplyConf(cfg, changes, next_idx);
            } else {
                return r.error();
            }
        }

        if (const auto r = Changer(tracker).EnterJoint(cs.auto_leave(), incoming)) {
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
