#include "raftpp/raft.h"

#include <ranges>

#include "spdlog/spdlog.h"

namespace raftpp {

bool UncommittedState::IsNoLimit() const {
    return max_uncommitted_size == std::numeric_limits<size_t>::max();
}

bool UncommittedState::MaybeIncreaseUncommittedSize(std::span<const Entry> entries) {
    if (IsNoLimit()) {
        return true;
    }

    const std::size_t size = std::transform_reduce(
        entries.begin(), entries.end(),
        std::size_t{0}, std::plus<>{},
        [](const Entry& e) { return e.data().size(); }
    );

    if (size == 0 || uncommitted_size == 0 || size + uncommitted_size <= max_uncommitted_size) {
        uncommitted_size += size;
        return true;
    }

    return false;
}

bool UncommittedState::MaybeReduceUncommittedSize(std::span<const Entry> entries) {
    if (IsNoLimit() || entries.empty()) {
        return true;
    }

    const std::size_t size = std::ranges::fold_left(
        entries
        | std::views::drop_while([this](const Entry& e) {
            return e.index() <= last_log_tail_index;
        })
        | std::views::transform([](const Entry& e) {
            return e.data().size();
        }),
        std::size_t{0}, std::plus{}
    );

    if (size > uncommitted_size) {
        uncommitted_size = 0;
        return false;
    }

    uncommitted_size -= size;
    return true;
}

uint64_t RaftCore::term() const {
    return term_;
}

RaftLog& RaftCore::raft_log() {
    return raft_log_;
}

ProgressTracker& Raft::progress_tracker() {
    return progress_tracker_;
}

Result<Raft> NewRaft(Config config, std::unique_ptr<Storage> store) {
    if (const auto r = config.Validate(); !r) {
        return RaftError(InvalidConfigError{""});
    }
    const auto raft_state = store->InitialState();
    if (!raft_state) {
        return RaftError(raft_state.error());
    }

    auto& conf_state = raft_state->conf_state;
    auto& voters = conf_state.voters();
    auto& learners = conf_state.learners();

    Raft r;

    // SPDLOG_INFO(
    //     "new raft, term={}, commit={}, applied={}, last_index={}, last_term={}, peers={}"
    //     r.term(), r.raft_log().committed(), r.raft_log().applied(),
    //     r.raft_log().LastIndex(), r.raft_log().LastTerm(), r.progress_tracker().conf().voters
    // );

    return r;
}

}
