#pragma once

#include <stddef.h>
#include <stdint.h>

#include <limits>

#include "error.h"
#include "read_only.h"

namespace raftpp {

constexpr size_t kHeartbeatTick = 2;

struct Config {
    uint64_t id = 0;
    size_t election_tick = kHeartbeatTick * 10;
    size_t heartbeat_tick = kHeartbeatTick;
    uint64_t applied = 0;
    uint64_t max_size_per_message = 0;
    size_t max_inflight_messages = 256;
    bool check_quorum = false;
    bool pre_vote = false;
    size_t min_election_tick = 0;
    size_t max_election_tick = 0;
    ReadOnlyOption read_only_option = ReadOnlyOption::Safe;
    bool skip_broadcast_commit = false;
    bool batch_append = false;
    int64_t priority = 0;
    uint64_t max_uncommitted_size = std::numeric_limits<uint64_t>::max();
    uint64_t max_committed_size_per_ready = std::numeric_limits<uint64_t>::max();
    uint64_t max_apply_unpersisted_log_limit = 0;
    bool disable_proposal_forwarding = false;
    bool load_state_on_startup = false;

    [[nodiscard]] size_t MinElectionTick() const;
    [[nodiscard]] size_t MaxElectionTick() const;
    [[nodiscard]] Result<void> Validate() const;
};

[[nodiscard]] Config DefaultConfig();

}  // namespace raftpp
