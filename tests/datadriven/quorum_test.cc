#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>

#include <doctest/doctest.h>

#include "datadriven.h"
#include "raftpp/core/joint_conf.h"
#include "raftpp/core/majority_conf.h"

namespace raftpp {
namespace test {

// A mutable AckIndexer for testing
class TestAckIndexer final : public AckedIndexer {
  public:
    std::optional<Index> AckedIndex(uint64_t voter) const override {
        if (auto it = data_.find(voter); it != data_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    void Insert(uint64_t id, Index idx) { data_[id] = idx; }

    void Remove(uint64_t id) { data_.erase(id); }

    bool Contains(uint64_t id) const { return data_.count(id) != 0; }

    void Retain(const std::function<bool(uint64_t, Index)>& predicate) {
        for (auto it = data_.begin(); it != data_.end();) {
            if (!predicate(it->first, it->second)) {
                it = data_.erase(it);
                continue;
            }
            ++it;
        }
    }

  private:
    Map<uint64_t, Index> data_;
};

// Helper to format VoteResult as string
static std::string VoteResultToString(VoteResult r) {
    switch (r) {
        case VoteResult::Won:
            return "VoteWon";
        case VoteResult::Lost:
            return "VoteLost";
        case VoteResult::Pending:
            return "VotePending";
    }
    return "Unknown";
}

// Helper to format Index as string
static std::string IndexToString(const Index& idx) {
    // Special case: u64::MAX is displayed as infinity symbol
    if (idx.index == std::numeric_limits<uint64_t>::max()) {
        return "∞";
    }
    // In raft-rs, Index displays as just the index value when group_id is 0
    if (idx.group_id == 0) {
        return std::to_string(idx.index);
    }
    return std::to_string(idx.index) + "@" + std::to_string(idx.group_id);
}

// Describe a MajorityConfig with an AckIndexer (matching raft-rs format)
static std::string DescribeMajorityConfig(const MajorityConfig& cfg, const TestAckIndexer& l) {
    size_t n = cfg.size();
    if (n == 0) {
        // No newline for empty config - the committed index will follow directly
        return "<empty majority quorum>";
    }

    struct Tup {
        uint64_t id;
        std::optional<Index> idx;
        size_t bar;  // length of bar displayed for this Tup
    };

    // Collect all entries
    std::vector<Tup> info;
    for (uint64_t id : cfg) {
        info.push_back({id, l.AckedIndex(id), 0});
    }

    // Sort by index (ascending), then by id
    std::sort(info.begin(), info.end(), [](const Tup& a, const Tup& b) {
        uint64_t ai = a.idx ? a.idx->index : 0;
        uint64_t bi = b.idx ? b.idx->index : 0;
        if (ai != bi)
            return ai < bi;
        return a.id < b.id;
    });

    // Populate .bar so that the i-th largest commit index has bar i
    // Note: bar is only set when index strictly increases, otherwise stays at 0
    for (size_t i = 1; i < n; ++i) {
        uint64_t prev_idx = info[i - 1].idx ? info[i - 1].idx->index : 0;
        uint64_t curr_idx = info[i].idx ? info[i].idx->index : 0;
        if (prev_idx < curr_idx) {
            info[i].bar = i;
        }
        // Otherwise bar stays at default value of 0
    }

    // Sort by id for output
    std::sort(info.begin(), info.end(), [](const Tup& a, const Tup& b) { return a.id < b.id; });

    std::ostringstream buf;
    // Header line
    buf << std::string(n, ' ') << "    idx\n";

    for (const auto& tup : info) {
        if (tup.idx) {
            buf << std::string(tup.bar, 'x') << '>';
            buf << std::string(n - tup.bar, ' ');
            // Right-align index in 5 chars
            std::string idx_str = IndexToString(*tup.idx);
            buf << std::setw(6) << idx_str;
            buf << "    (id=" << tup.id << ")\n";
        } else {
            buf << '?' << std::string(n, ' ');
            buf << std::setw(6) << "0";
            buf << "    (id=" << tup.id << ")\n";
        }
    }

    return buf.str();
}

// Describe a JointConfiguration (uses MajorityConfig describe with all IDs)
static std::string DescribeJointConfig(const JointConfiguration& cfg, const TestAckIndexer& l) {
    // Create a MajorityConfig from all IDs
    MajorityConfig combined(cfg.IDs());
    return DescribeMajorityConfig(combined, l);
}

static std::string TestQuorum(const TestData& data) {
    // Parse configuration
    bool joint = false;
    std::vector<uint64_t> ids;
    std::vector<uint64_t> idsj;
    std::vector<Index> idxs;
    std::vector<uint64_t> gids;
    std::vector<Index> votes;

    for (const auto& arg : data.cmd_args) {
        for (const auto& val : arg.vals) {
            if (arg.key == "cfg") {
                ids.push_back(std::stoull(val));
            } else if (arg.key == "cfgj") {
                joint = true;
                if (val != "zero") {
                    idsj.push_back(std::stoull(val));
                }
            } else if (arg.key == "idx") {
                uint64_t n = 0;
                if (val != "_") {
                    n = std::stoull(val);
                    if (n == 0) {
                        return "error: use '_' as 0, check " + data.pos + "\n";
                    }
                }
                idxs.push_back({n, 0});
            } else if (arg.key == "gid") {
                uint64_t n = 0;
                if (val != "_") {
                    n = std::stoull(val);
                    if (n == 0) {
                        return "error: use '_' as 0, check " + data.pos + "\n";
                    }
                }
                gids.push_back(n);
            } else if (arg.key == "votes") {
                if (val == "y") {
                    votes.push_back({2, 0});
                } else if (val == "n") {
                    votes.push_back({1, 0});
                } else if (val == "_") {
                    votes.push_back({0, 0});
                } else {
                    return "error: unknown vote value: " + val + "\n";
                }
            } else {
                return "error: unknown arg: " + arg.key + "\n";
            }
        }
    }

    // Build configs
    Set<uint64_t> ids_set(ids.begin(), ids.end());
    Set<uint64_t> idsj_set(idsj.begin(), idsj.end());

    MajorityConfig c(ids_set);
    MajorityConfig cj(idsj_set);

    // Helper to build lookuper
    auto make_lookuper = [](const std::vector<Index>& idxs, const std::vector<uint64_t>& ids,
                            const std::vector<uint64_t>& idsj) -> TestAckIndexer {
        TestAckIndexer l;
        size_t p = 0;

        // Chain ids and idsj
        std::vector<uint64_t> all_ids;
        all_ids.insert(all_ids.end(), ids.begin(), ids.end());
        all_ids.insert(all_ids.end(), idsj.begin(), idsj.end());

        for (uint64_t id : all_ids) {
            if (!l.Contains(id) && p < idxs.size()) {
                l.Insert(id, idxs[p]);
                p++;
            }
        }

        // Remove zero entries
        l.Retain([](uint64_t, Index idx) { return idx.index > 0; });

        return l;
    };

    // Verify input length
    size_t input_len = data.cmd == "vote" ? votes.size() : idxs.size();

    JointConfiguration jc(ids_set, idsj_set);
    size_t voters_count = jc.IDs().size();

    if (voters_count != input_len) {
        return "error: mismatched input (explicit or _) for voters " +
            std::to_string(voters_count) + ": " + std::to_string(input_len) + "\n";
    }

    // Verify group ids length
    if (!gids.empty()) {
        if (gids.size() != voters_count) {
            return "error: mismatched input (explicit or _) for group ids " +
                std::to_string(voters_count) + ": " + std::to_string(gids.size()) + "\n";
        }
        // Assign group ids
        for (size_t i = 0; i < idxs.size() && i < gids.size(); ++i) {
            idxs[i].group_id = gids[i];
        }
    }

    std::ostringstream buf;

    if (data.cmd == "committed") {
        bool use_group_commit = false;
        auto l = make_lookuper(idxs, ids, idsj);

        std::pair<uint64_t, bool> idx;

        if (joint) {
            JointConfiguration cc(ids_set, idsj_set);
            buf << DescribeJointConfig(cc, l);
            idx = cc.CommittedIndex(use_group_commit, l);

            // Check symmetry
            JointConfiguration cc_swap(idsj_set, ids_set);
            auto a_idx = cc_swap.CommittedIndex(use_group_commit, l);
            if (a_idx.first != idx.first) {
                buf << a_idx.first << " <-- via symmetry\n";
            }
        } else {
            idx = c.CommittedIndex(use_group_commit, l);
            buf << DescribeMajorityConfig(c, l);

            // Test with empty joint config
            JointConfiguration cc_zero(ids_set, {});
            auto a_idx = cc_zero.CommittedIndex(use_group_commit, l);
            if (a_idx.first != idx.first) {
                buf << a_idx.first << " <-- via zero-joint quorum\n";
            }

            // Test self-joint
            JointConfiguration cc_self(ids_set, ids_set);
            a_idx = cc_self.CommittedIndex(use_group_commit, l);
            if (a_idx.first != idx.first) {
                buf << a_idx.first << " <-- via self-joint quorum\n";
            }

            // Test overlaying
            for (uint64_t id : c) {
                auto iidx = l.AckedIndex(id);
                if (iidx && idx.first > iidx->index) {
                    // Try index - 1
                    l.Insert(id, {iidx->index - 1, iidx->group_id});
                    a_idx = c.CommittedIndex(use_group_commit, l);
                    if (a_idx.first != idx.first) {
                        buf << a_idx.first << " <-- overlaying " << id << "->" << (iidx->index - 1)
                            << "\n";
                    }

                    // Try 0
                    l.Insert(id, {0, iidx->group_id});
                    a_idx = c.CommittedIndex(use_group_commit, l);
                    if (a_idx.first != idx.first) {
                        buf << a_idx.first << " <-- overlaying " << id << "->0\n";
                    }

                    // Restore
                    l.Insert(id, *iidx);
                }
            }
        }

        buf << IndexToString({idx.first, 0}) << "\n";

    } else if (data.cmd == "group_committed") {
        bool use_group_commit = true;
        auto l = make_lookuper(idxs, ids, idsj);

        std::pair<uint64_t, bool> idx = {0, false};

        if (joint) {
            JointConfiguration cc(ids_set, idsj_set);
            idx = cc.CommittedIndex(use_group_commit, l);

            // Check symmetry
            JointConfiguration cc_swap(idsj_set, ids_set);
            auto a_idx = cc_swap.CommittedIndex(use_group_commit, l);
            if (a_idx.first != idx.first) {
                buf << a_idx.first << " <-- via symmetry\n";
            }
        }

        buf << IndexToString({idx.first, 0}) << "\n";

    } else if (data.cmd == "vote") {
        auto ll = make_lookuper(votes, ids, idsj);
        Map<uint64_t, bool> vote_map;

        // Build vote map from lookuper
        std::vector<uint64_t> all_ids;
        all_ids.insert(all_ids.end(), ids.begin(), ids.end());
        all_ids.insert(all_ids.end(), idsj.begin(), idsj.end());
        Set<uint64_t> seen;
        size_t p = 0;
        for (uint64_t id : all_ids) {
            if (seen.count(id) == 0 && p < votes.size()) {
                if (votes[p].index > 0) {  // Non-pending
                    vote_map[id] = (votes[p].index != 1);
                }
                seen.insert(id);
                p++;
            }
        }

        auto check = [&vote_map](uint64_t id) -> std::optional<bool> {
            if (auto it = vote_map.find(id); it != vote_map.end()) {
                return it->second;
            }
            return std::nullopt;
        };

        VoteResult r;
        if (joint) {
            JointConfiguration cc(ids_set, idsj_set);
            r = cc.GetVoteResult(check);

            // Check symmetry
            JointConfiguration cc_swap(idsj_set, ids_set);
            auto ar = cc_swap.GetVoteResult(check);
            if (ar != r) {
                buf << VoteResultToString(ar) << " <-- via symmetry\n";
            }
        } else {
            r = c.GetVoteResult(check);
        }

        buf << VoteResultToString(r) << "\n";
    } else {
        return "error: unknown command: " + data.cmd + "\n";
    }

    return buf.str();
}

TEST_CASE("quorum datadriven") {
    RunTest(TESTDATA_DIR "/quorum", TestQuorum);
}

}  // namespace test
}  // namespace raftpp
