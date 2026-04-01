#include "raftpp/core/conf_restore.h"

#include <utility>
#include <vector>

#include <nonstd/span.hpp>

#include "raftpp.capnp.h"
#include "raftpp/core/capnp_util.h"
#include "raftpp/core/conf_changer.h"
#include "raftpp/core/progress_tracker.h"
#include "raftpp/core/tracker_conf.h"
#include "raftpp/core/types.h"

namespace raftpp {

std::pair<std::vector<ConfChangeSingle>, std::vector<ConfChangeSingle>> ToConfChangeSingle(
    const ConfState& cs
) {
    std::vector<ConfChangeSingle> outgoing;
    std::vector<ConfChangeSingle> incoming;

    auto cs_reader = capnp_util::reader<msg::ConfState>(cs);

    for (const uint64_t id : cs_reader.getVotersOutgoing()) {
        ConfChangeSingle s = capnp_util::make<msg::ConfChangeSingle>();
        auto builder = capnp_util::builder<msg::ConfChangeSingle>(s);
        builder.setNodeId(id);
        builder.setChangeType(capnp_util::cast_enum<msg::ConfChangeType>(ConfChangeType::ADD_NODE));
        outgoing.emplace_back(std::move(s));
    }

    for (const uint64_t id : cs_reader.getVotersOutgoing()) {
        ConfChangeSingle s = capnp_util::make<msg::ConfChangeSingle>();
        auto builder = capnp_util::builder<msg::ConfChangeSingle>(s);
        builder.setNodeId(id);
        builder.setChangeType(
            capnp_util::cast_enum<msg::ConfChangeType>(ConfChangeType::REMOVE_NODE)
        );
        incoming.emplace_back(std::move(s));
    }

    for (const uint64_t id : cs_reader.getVoters()) {
        ConfChangeSingle s = capnp_util::make<msg::ConfChangeSingle>();
        auto builder = capnp_util::builder<msg::ConfChangeSingle>(s);
        builder.setNodeId(id);
        builder.setChangeType(capnp_util::cast_enum<msg::ConfChangeType>(ConfChangeType::ADD_NODE));
        incoming.emplace_back(std::move(s));
    }

    for (const uint64_t id : cs_reader.getLearners()) {
        ConfChangeSingle s = capnp_util::make<msg::ConfChangeSingle>();
        auto builder = capnp_util::builder<msg::ConfChangeSingle>(s);
        builder.setNodeId(id);
        builder.setChangeType(
            capnp_util::cast_enum<msg::ConfChangeType>(ConfChangeType::ADD_LEARNER_NODE)
        );
        incoming.emplace_back(std::move(s));
    }

    for (const uint64_t id : cs_reader.getLearnersNext()) {
        ConfChangeSingle s = capnp_util::make<msg::ConfChangeSingle>();
        auto builder = capnp_util::builder<msg::ConfChangeSingle>(s);
        builder.setNodeId(id);
        builder.setChangeType(
            capnp_util::cast_enum<msg::ConfChangeType>(ConfChangeType::ADD_LEARNER_NODE)
        );
        incoming.emplace_back(std::move(s));
    }

    return std::make_pair(std::move(outgoing), std::move(incoming));
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
                if (const auto r = changer.Apply(cfg, prs, nonstd::span{&i, 1}); !r) {
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

        if (const auto r = ConfChanger(tracker).EnterJoint(
                capnp_util::reader<msg::ConfState>(cs).getAutoLeave(), incoming
            )) {
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
