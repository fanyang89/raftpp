#include "raftpp/core/tracker_conf.h"

namespace raftpp {

TrackerConfiguration::TrackerConfiguration() : auto_leave(false) {}

TrackerConfiguration::TrackerConfiguration(
    const Set<uint64_t>& voters, const Set<uint64_t>& /*learners*/
)
    : voters(voters), auto_leave(false) {}

void TrackerConfiguration::Clear() {
    voters.Clear();
    learners.clear();
    learners_next.clear();
    auto_leave = false;
}

ConfState TrackerConfiguration::ToConfState() {
    ConfState cs = capnp_util::make<msg::ConfState>();
    auto builder = capnp_util::builder<msg::ConfState>(cs);

    // Convert voters incoming to a vector first
    auto incoming = voters.incoming();
    auto voters_builder = builder.initVoters(incoming.size());
    size_t i = 0;
    for (const auto v : incoming) {
        voters_builder.set(i++, v);
    }

    // Convert voters outgoing
    auto outgoing = voters.outgoing();
    auto voters_out_builder = builder.initVotersOutgoing(outgoing.size());
    i = 0;
    for (const auto v : outgoing) {
        voters_out_builder.set(i++, v);
    }

    // Convert learners
    auto learners_builder = builder.initLearners(learners.size());
    i = 0;
    for (const auto v : learners) {
        learners_builder.set(i++, v);
    }

    // Convert learners_next
    auto learners_next_builder = builder.initLearnersNext(learners_next.size());
    i = 0;
    for (const auto v : learners_next) {
        learners_next_builder.set(i++, v);
    }

    builder.setAutoLeave(auto_leave);
    return cs;
}

}  // namespace raftpp

fmt::context::iterator fmt::formatter<raftpp::TrackerConfiguration>::format(
    const raftpp::TrackerConfiguration& value, const format_context& ctx
) {
    return fmt::format_to(
        ctx.out(), "[voters: {}, learners: ({}), learners_next: ({}), auto_leave: {}]",
        value.voters, fmt::join(value.learners, " "), fmt::join(value.learners_next, " "),
        value.auto_leave
    );
}
