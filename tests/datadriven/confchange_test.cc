#include <doctest/doctest.h>

#include <algorithm>
#include <sstream>

#include "datadriven.h"
#include "raftpp/conf_changer.h"
#include "raftpp/progress_tracker.h"

namespace raftpp {
namespace test {

// Extract error message from RaftError (without prefix)
static std::string ExtractErrorMessage(const RaftError& err) {
    // RaftError ToString returns "conf change error: message"
    // We need just "message"
    std::string full = err.ToString();
    // Find the colon and skip it plus the space
    auto pos = full.find(": ");
    if (pos != std::string::npos) {
        return full.substr(pos + 2);
    }
    return full;
}

// Parse conf change input like "v1 v2 l3 r4" into ConfChangeSingle vector
static std::vector<ConfChangeSingle> ParseConfChange(const std::string& input) {
    std::vector<ConfChangeSingle> result;

    std::istringstream iss(input);
    std::string token;

    while (iss >> token) {
        if (token.length() < 2) {
            throw std::runtime_error("unknown token " + token);
        }

        ConfChangeSingle cc;
        char op = token[0];
        std::string id_str = token.substr(1);

        switch (op) {
            case 'v':
                cc.set_change_type(ConfChangeType::AddNode);
                break;
            case 'l':
                cc.set_change_type(ConfChangeType::AddLearnerNode);
                break;
            case 'r':
                cc.set_change_type(ConfChangeType::RemoveNode);
                break;
            default:
                throw std::runtime_error("unknown token " + token);
        }

        cc.set_node_id(std::stoull(id_str));
        result.push_back(cc);
    }

    return result;
}

// Format TrackerConfiguration for test output
static std::string FormatConfiguration(const TrackerConfiguration& cfg) {
    std::ostringstream oss;

    // Voters
    if (cfg.voters.outgoing().empty()) {
        // Simple config: voters=(1 2 3)
        std::vector<uint64_t> incoming(cfg.voters.incoming().begin(), cfg.voters.incoming().end());
        std::sort(incoming.begin(), incoming.end());
        oss << "voters=(";
        for (size_t i = 0; i < incoming.size(); ++i) {
            if (i > 0) oss << " ";
            oss << incoming[i];
        }
        oss << ")";
    } else {
        // Joint config: voters=(1 2)&&(2 3)
        std::vector<uint64_t> incoming(cfg.voters.incoming().begin(), cfg.voters.incoming().end());
        std::vector<uint64_t> outgoing(cfg.voters.outgoing().begin(), cfg.voters.outgoing().end());
        std::sort(incoming.begin(), incoming.end());
        std::sort(outgoing.begin(), outgoing.end());

        oss << "voters=(";
        for (size_t i = 0; i < incoming.size(); ++i) {
            if (i > 0) oss << " ";
            oss << incoming[i];
        }
        oss << ")&&(";
        for (size_t i = 0; i < outgoing.size(); ++i) {
            if (i > 0) oss << " ";
            oss << outgoing[i];
        }
        oss << ")";
    }

    // Learners
    if (!cfg.learners.empty()) {
        std::vector<uint64_t> learners(cfg.learners.begin(), cfg.learners.end());
        std::sort(learners.begin(), learners.end());
        oss << " learners=(";
        for (size_t i = 0; i < learners.size(); ++i) {
            if (i > 0) oss << " ";
            oss << learners[i];
        }
        oss << ")";
    }

    // Learners next
    if (!cfg.learners_next.empty()) {
        std::vector<uint64_t> learners_next(cfg.learners_next.begin(), cfg.learners_next.end());
        std::sort(learners_next.begin(), learners_next.end());
        oss << " learners_next=(";
        for (size_t i = 0; i < learners_next.size(); ++i) {
            if (i > 0) oss << " ";
            oss << learners_next[i];
        }
        oss << ")";
    }

    // Auto leave
    if (cfg.auto_leave) {
        oss << " autoleave";
    }

    return oss.str();
}

// Format ProgressState for output
static std::string FormatProgressState(ProgressState state) {
    switch (state) {
        case ProgressState::Probe:
            return "StateProbe";
        case ProgressState::Replicate:
            return "StateReplicate";
        case ProgressState::Snapshot:
            return "StateSnapshot";
    }
    return "Unknown";
}

TEST_CASE("confchange datadriven") {
    Walk(TESTDATA_DIR "/confchange", [](const std::filesystem::path& path) {
        // Create a fresh ProgressTracker for each file
        ProgressTracker tr(10);
        uint64_t idx = 0;

        RunTest(
            path,
            [&tr, &idx](const TestData& data) -> std::string {
                // Parse input
                auto ccs = ParseConfChange(data.input);

                // Execute command
                Result<std::pair<TrackerConfiguration, MapChange>> res;

                if (data.cmd == "simple") {
                    res = ConfChanger(tr).Simple(ccs);
                } else if (data.cmd == "enter-joint") {
                    bool auto_leave = false;
                    for (const auto& arg : data.cmd_args) {
                        if (arg.key == "autoleave" && !arg.vals.empty()) {
                            auto_leave = (arg.vals[0] == "true");
                        }
                    }
                    res = ConfChanger(tr).EnterJoint(auto_leave, ccs);
                } else if (data.cmd == "leave-joint") {
                    res = ConfChanger(tr).LeaveJoint();
                } else {
                    return "unknown command: " + data.cmd + "\n";
                }

                if (!res) {
                    idx++;
                    // Extract error message
                    return ExtractErrorMessage(res.error()) + "\n";
                }

                // Apply the configuration
                auto& [conf, changes] = *res;
                tr.ApplyConf(conf, changes, idx);
                idx++;

                // Format output
                std::ostringstream buf;

                // Configuration line
                buf << FormatConfiguration(tr.conf()) << "\n";

                // Progress entries (sorted by id)
                std::vector<std::pair<uint64_t, const Progress*>> prs;
                for (const auto& [id, pr] : tr.progress_map()) {
                    prs.emplace_back(id, &pr);
                }
                std::sort(prs.begin(), prs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

                for (const auto& [id, pr] : prs) {
                    buf << id << ": " << FormatProgressState(pr->state()) << " match=" << pr->matched()
                        << " next=" << pr->next_idx();

                    // Add learner marker
                    if (tr.conf().learners.contains(id)) {
                        buf << " learner";
                    }
                    buf << "\n";
                }

                return buf.str();
            },
            false);
    });
}

}  // namespace test
}  // namespace raftpp
