#include "raftpp/tracker_conf.h"

namespace raftpp {

TrackerConfiguration::TrackerConfiguration() = default;

TrackerConfiguration::TrackerConfiguration(const Set<uint64_t>& voters, const Set<uint64_t>& learners)
    : voters(voters), auto_leave(false) {}

void TrackerConfiguration::Clear() {
    voters.Clear();
    learners.clear();
    learners_next.clear();
    auto_leave = false;
}

ConfState TrackerConfiguration::ToConfState() {
    ConfState cs;

    for (const auto v : voters.incoming()) {
        cs.mutable_voters()->Add(v);
    }

    for (const auto v : voters.outgoing()) {
        cs.mutable_voters_outgoing()->Add(v);
    }

    for (const auto v : learners) {
        cs.mutable_learners()->Add(v);
    }

    for (const auto v : learners_next) {
        cs.mutable_learners_next()->Add(v);
    }

    cs.set_auto_leave(auto_leave);
    return cs;
}

void to_json(nlohmann::json& j, const TrackerConfiguration& p) {
    j["voters"] = p.voters;
    j["learners"] = p.learners;
    j["learners_next"] = p.learners_next;
    j["auto_leave"] = p.auto_leave;
}

void from_json(const nlohmann::json& j, TrackerConfiguration& p) {
    j.at("voters").get_to(p.voters);
    j.at("learners").get_to(p.learners);
    j.at("learners_next").get_to(p.learners_next);
    j.at("auto_leave").get_to(p.auto_leave);
}

}  // namespace raftpp

fmt::context::iterator fmt::formatter<raftpp::TrackerConfiguration>::format(
    const raftpp::TrackerConfiguration& value, const format_context& ctx
) {
    const nlohmann::json j = value;
    return fmt::format_to(ctx.out(), "{}", j.dump());
}
