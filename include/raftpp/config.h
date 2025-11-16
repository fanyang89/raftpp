#pragma once

#include "raftpp/readonly.h"
#include "raftpp/error.h"

namespace raftpp {

struct Config {
    uint64_t id;
    size_t election_tick;
    size_t heartbeat_tick;
    uint64_t applied;
    uint64_t max_size_per_message;
    size_t max_inflight_messages;
    bool check_quorum;
    bool pre_vote;
    size_t min_election_tick;
    size_t max_election_tick;
    ReadOnlyOption read_only_option;
    bool skip_broadcast_commit;
    bool batch_append;
    int64_t priority;
    uint64_t max_uncommitted_size;
    uint64_t max_committed_size_per_ready;
    uint64_t max_apply_unpersisted_log_limit;
    bool disable_proposal_forwarding;

    size_t MinElectionTick() const;
    size_t MaxElectionTick() const;

    Result<void> Validate();
};

}
