#include "raftpp/majority.h"

#include <algorithm>

#include <libassert/assert.hpp>
#include <nlohmann/json.hpp>

namespace raftpp {

size_t majority(const size_t total) {
    return total / 2 + 1;
}

MajorityConfig::MajorityConfig() = default;

MajorityConfig::MajorityConfig(const Set<uint64_t>& voters) {
    insert(voters.begin(), voters.end());
}

std::pair<uint64_t, bool> MajorityConfig::CommittedIndex(const bool use_group_commit, AckedIndexer& l) const {
    if (empty()) {
        return std::make_pair(std::numeric_limits<uint64_t>::max(), true);
    }

    std::vector<Index> matched;
    matched.reserve(size());
    for (const auto voter : *this) {
        auto x = l.AckedIndex(voter);
        ASSERT(x.has_value());
        matched.emplace_back(*x);
    }
    std::ranges::sort(matched, [](const Index lhs, const Index rhs) { return lhs.index > rhs.index; });

    const size_t quorum = majority(matched.size());
    const auto quorum_index = matched[quorum - 1];
    if (!use_group_commit) {
        return std::make_pair(quorum_index.index, false);
    }

    const auto quorum_commit_index = quorum_index.index;
    auto checked_group_id = quorum_index.group_id;
    bool single_group = true;
    for (const Index& m : matched) {
        if (m.group_id == 0) {
            single_group = false;
            continue;
        }
        if (checked_group_id == 0) {
            checked_group_id = m.group_id;
            continue;
        }
        if (checked_group_id == m.group_id) {
            continue;
        }
        return std::make_pair(std::min(m.index, quorum_commit_index), true);
    }

    if (single_group) {
        return std::make_pair(quorum_index.index, false);
    }
    return std::make_pair(matched.back().index, false);
}

VoteResult MajorityConfig::GetVoteResult(const std::function<std::optional<bool>(uint64_t)>& check) const {
    if (empty()) {
        return VoteResult::Won;
    }

    size_t yes = 0;
    size_t missing = 0;

    for (const uint64_t voter : *this) {
        if (auto r = check(voter); r.has_value()) {
            if (r.value()) {
                yes++;
            }
        } else {
            ++missing;
        }
    }

    const size_t q = majority(size());
    if (yes >= q) {
        return VoteResult::Won;
    }
    if (yes + missing >= q) {
        return VoteResult::Pending;
    }
    return VoteResult::Lost;
}

void to_json(nlohmann::json& j, const MajorityConfig& p) {
    j["voters"] = Set<uint64_t>(p);
}

void from_json(const nlohmann::json& j, MajorityConfig& p) {
    j.at("voters").get_to(p);
}

}  // namespace raftpp

fmt::context::iterator fmt::formatter<raftpp::MajorityConfig>::format(
    const raftpp::MajorityConfig& value, const format_context& ctx
) {
    const nlohmann::json j = value;
    return fmt::format_to(ctx.out(), "{}", j.dump());
}
