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
        entries.begin(), entries.end(), std::size_t{0}, std::plus{}, [](const Entry& e) { return e.data().size(); }
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
        entries | std::views::drop_while([this](const Entry& e) { return e.index() <= last_log_tail_index; }) |
            std::views::transform([](const Entry& e) { return e.data().size(); }),
        std::size_t{0}, std::plus{}
    );

    if (size > uncommitted_size) {
        uncommitted_size = 0;
        return false;
    }

    uncommitted_size -= size;
    return true;
}

Raft::Raft(const Config& config, std::unique_ptr<Storage> store)
    : RaftCore(config, std::move(store)), progress_tracker_(config.max_inflight_messages), config_(config) {
    if (const auto r = config.Validate(); !r) {
        PANIC(r.error());
    }
    const auto raft_state = store->InitialState();
    if (!raft_state) {
        PANIC(raft_state.error());
    }

    auto& conf_state = raft_state->conf_state;
    auto& voters = conf_state.voters();
    auto& learners = conf_state.learners();
}

ProgressTracker& Raft::progress_tracker() {
    return progress_tracker_;
}

const ProgressTracker& Raft::progress_tracker() const {
    return progress_tracker_;
}

}  // namespace raftpp
