#pragma once

#include "raftpp/error.h"
#include "raftpp/read_only.h"

namespace raftpp {

constexpr size_t HEARTBEAT_TICK = 2;

struct Config {
    uint64_t id;
    size_t election_tick = HEARTBEAT_TICK * 10;
    size_t heartbeat_tick = HEARTBEAT_TICK;
    uint64_t applied;
    uint64_t max_size_per_message;
    size_t max_inflight_messages = 256;
    bool check_quorum;
    bool pre_vote;
    size_t min_election_tick;
    size_t max_election_tick;
    ReadOnlyOption read_only_option = ReadOnlyOption::Safe;
    bool skip_broadcast_commit;
    bool batch_append;
    int64_t priority;
    uint64_t max_uncommitted_size = std::numeric_limits<uint64_t>::max();
    uint64_t max_committed_size_per_ready = std::numeric_limits<uint64_t>::max();
    uint64_t max_apply_unpersisted_log_limit;
    bool disable_proposal_forwarding;

    size_t MinElectionTick() const;
    size_t MaxElectionTick() const;
    Result<void> Validate() const;
};

}  // namespace raftpp
