#include "raftpp/joint.h"

namespace raftpp {

JointConfiguration::JointConfiguration() = default;

JointConfiguration::JointConfiguration(const Set<uint64_t>& voters) : incoming_(voters) {}

JointConfiguration::JointConfiguration(const Set<uint64_t>& incoming, const Set<uint64_t>& outgoing)
    : incoming_(incoming), outgoing_(outgoing) {}

std::pair<uint64_t, bool> JointConfiguration::CommittedIndex(const bool use_group_commit, AckedIndexer& l) const {
    const auto [i_idx, i_use_group_commit] = incoming_.CommittedIndex(use_group_commit, l);
    const auto [o_idx, o_use_group_commit] = outgoing_.CommittedIndex(use_group_commit, l);
    return {std::min(i_idx, o_idx), i_use_group_commit && o_use_group_commit};
}

VoteResult JointConfiguration::GetVoteResult(const std::function<std::optional<bool>(uint64_t)>& check) const {
    const auto in = incoming_.GetVoteResult(check);
    const auto out = outgoing_.GetVoteResult(check);

    if (in == VoteResult::Won && out == VoteResult::Won) {
        return VoteResult::Won;
    }

    if (in == VoteResult::Lost || out == VoteResult::Lost) {
        return VoteResult::Lost;
    }

    return VoteResult::Pending;
}

void JointConfiguration::Clear() {
    incoming_.Clear();
    outgoing_.Clear();
}

bool JointConfiguration::Contains(const uint64_t id) const {
    return incoming_.Contains(id) || outgoing_.Contains(id);
}

}  // namespace raftpp
