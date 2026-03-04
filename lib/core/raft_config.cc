#include "raftpp/core/raft_config.h"

#include <spdlog/fmt/fmt.h>

namespace raftpp {

size_t Config::MinElectionTick() const {
    if (min_election_tick == 0) {
        return election_tick;
    }
    return min_election_tick;
}

size_t Config::MaxElectionTick() const {
    if (max_election_tick == 0) {
        return 2 * election_tick;
    }
    return max_election_tick;
}

Result<void> Config::Validate() const {
    if (id == kInvalidId) {
        return RaftError(ConfigErrorCode::InvalidNodeId);
    }
    if (heartbeat_tick == 0) {
        return RaftError(ConfigErrorCode::HeartbeatTickTooSmall);
    }
    if (election_tick <= heartbeat_tick) {
        return RaftError(ConfigErrorCode::ElectionTickTooSmall);
    }

    const size_t min_timeout = MinElectionTick();
    const size_t max_timeout = MaxElectionTick();

    if (min_timeout < election_tick) {
        return InvalidConfigError(
                   fmt::format(
                       "min election tick {} must not be less than election_tick {}", min_timeout,
                       election_tick
                   )
        )
            .ToError();
    }

    if (min_timeout >= max_timeout) {
        return InvalidConfigError(
                   fmt::format(
                       "min election tick {} should be less than max election tick {}", min_timeout,
                       max_timeout
                   )
        )
            .ToError();
    }

    if (max_inflight_messages == 0) {
        return RaftError(ConfigErrorCode::MaxInflightMessagesTooSmall);
    }

    if (read_only_option == ReadOnlyOption::LeaseBased && !check_quorum) {
        return RaftError(ConfigErrorCode::LeaseBasedReadRequiresCheckQuorum);
    }

    if (max_uncommitted_size < max_size_per_message) {
        return InvalidConfigError("max uncommitted size should greater than max_size_per_msg")
            .ToError();
    }

    return {};
}

Config DefaultConfig() {
    Config c;
    c.id = 0;
    c.applied = 0;
    c.max_size_per_message = 0;
    c.check_quorum = false;
    c.pre_vote = false;
    c.min_election_tick = 0;
    c.max_election_tick = 0;
    c.skip_broadcast_commit = false;
    c.batch_append = false;
    c.priority = 0;
    c.max_apply_unpersisted_log_limit = 0;
    c.disable_proposal_forwarding = false;
    return c;
}

}  // namespace raftpp
