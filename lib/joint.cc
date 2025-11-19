#include "raftpp/joint.h"

namespace raftpp {

JointConfiguration::JointConfiguration() = default;

JointConfiguration::JointConfiguration(const Set<uint64_t>& voters) : incoming_(voters) {}

JointConfiguration::JointConfiguration(const Set<uint64_t>& incoming, const Set<uint64_t>& outgoing)
    : incoming_(incoming), outgoing_(outgoing) {}

std::pair<uint64_t, bool> JointConfiguration::CommittedIndex(const bool use_group_commit, const AckedIndexer& l) const {
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
    incoming_.clear();
    outgoing_.clear();
}

bool JointConfiguration::Contains(const uint64_t id) const {
    return incoming_.contains(id) || outgoing_.contains(id);
}

Set<uint64_t> JointConfiguration::IDs() const {
    Set<uint64_t> ids(incoming());
    ids.insert(outgoing_.begin(), outgoing_.end());
    return ids;
}

MajorityConfig& JointConfiguration::outgoing() {
    return outgoing_;
}

const MajorityConfig& JointConfiguration::outgoing() const {
    return outgoing_;
}

MajorityConfig& JointConfiguration::incoming() {
    return incoming_;
}

const MajorityConfig& JointConfiguration::incoming() const {
    return incoming_;
}

void to_json(nlohmann::json& j, const JointConfiguration& p) {
    j["outgoing"] = p.outgoing();
    j["incoming"] = p.incoming();
}

void from_json(const nlohmann::json& j, JointConfiguration& p) {
    j.at("outgoing").get_to(p.outgoing());
    j.at("incoming").get_to(p.incoming());
}

}  // namespace raftpp

fmt::context::iterator fmt::formatter<raftpp::JointConfiguration>::format(
    const raftpp::JointConfiguration& value, const format_context& ctx
) {
    const nlohmann::json j = value;
    return fmt::format_to(ctx.out(), "{}", j.dump());
}
