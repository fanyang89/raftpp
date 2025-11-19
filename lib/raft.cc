#include "raftpp/raft.h"

#include <random>
#include <ranges>

#include <google/protobuf/util/message_differencer.h>

#include "raftpp/conf_restore.h"
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

    if (const auto r = Restore(progress_tracker_, raft_log_.LastIndex(), conf_state); !r) {
        PANIC("Configuration restore failed, err: {}", r.error());
    }

    if (const ConfState new_cs = PostConfChange();
        !google::protobuf::util::MessageDifferencer::Equals(conf_state, new_cs)) {
        PANIC("invalid restore: {} != {}", conf_state.DebugString(), new_cs.DebugString());
    }

    if (!google::protobuf::util::MessageDifferencer::Equals(raft_state->hard_state, HardState::default_instance())) {
        LoadState(raft_state->hard_state);
    }

    if (config.applied > 0) {
        CommitApplyInternal(config.applied, true);
    }

    BecomeFollower(term_, INVALID_ID);

    RaftLog& log = raft_log_;
    SPDLOG_INFO(
        "new raft instance, term={}, commit={}, applied={}, last_index={}, last_term={}, peers={}", term_,
        log.committed(), log.applied(), log.LastIndex(), log.LastTerm(),
        fmt::format("{}", progress_tracker_.conf().voters)
    );
}

ProgressTracker& Raft::progress_tracker() {
    return progress_tracker_;
}

const ProgressTracker& Raft::progress_tracker() const {
    return progress_tracker_;
}

void Raft::ResetRandomizedElectionTimeout() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution dist(min_election_timeout_, max_election_timeout_);
    const size_t timeout = dist(gen);
    size_t prev_timeout = randomized_election_timeout_;
    randomized_election_timeout_ = timeout;
    SPDLOG_INFO("reset election timeout, {} -> {}", prev_timeout, timeout);
}

void Raft::Reset(uint64_t term) {
    if (term_ != term) {
        term_ = term;
        vote_ = INVALID_ID;
    }
    leader_id_ = INVALID_ID;
    ResetRandomizedElectionTimeout();
    election_elapsed_ = 0;
    heartbeat_elapsed_ = 0;

    AbortLeaderTransfer();
    progress_tracker_.ResetVotes();

    pending_conf_index_ = 0;
    read_only_ = ReadOnly(read_only_.option());
    pending_request_snapshot_ = INVALID_INDEX;

    const uint64_t last_index = raft_log_.LastIndex();
    const uint64_t committed = raft_log_.committed();
    const uint64_t persisted = raft_log_.persisted();
    const uint64_t self_id = id_;
    for (auto& [id, pr] : progress_tracker_.progress_map()) {
        pr.Reset(last_index + 1);
        if (id == self_id) {
            pr.matched() = persisted;
            pr.committed_index() = committed;
        }
    }
}

void Raft::BecomeFollower(const uint64_t term, const uint64_t leader_id) {
    const uint64_t pending_request_snapshot = pending_request_snapshot_;
    Reset(term);
    leader_id_ = leader_id;
    const auto from_role = state_;
    state_ = StateRole::Follower;
    pending_request_snapshot_ = pending_request_snapshot;
    raft_log_.max_apply_unpersisted_log_limit() = 0;

    SPDLOG_INFO("became follower, term={}, from_role={}", term, magic_enum::enum_name(from_role));
}

}  // namespace raftpp
