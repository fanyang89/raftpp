#include "raftpp/config.h"

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
    if (id == INVALID_ID) {
        return InvalidConfigError("invalid node id").ToError();
    }
    if (heartbeat_tick == 0) {
        return InvalidConfigError("heartbeat tick must greater than 0").ToError();
    }
    if (election_tick <= heartbeat_tick) {
        return InvalidConfigError("election tick must be greater than heartbeat tick").ToError();
    }

    const size_t min_timeout = MinElectionTick();
    const size_t max_timeout = MaxElectionTick();

    if (min_timeout < election_tick) {
        return InvalidConfigError(
            fmt::format(
                "min election tick {} must not be less than election_tick {}",
                min_timeout, election_tick)).ToError();
    }

    if (min_timeout >= max_timeout) {
        return InvalidConfigError(fmt::format(
            "min election tick {} should be less than max election tick {}",
            min_timeout, max_timeout)).ToError();
    }

    if (max_inflight_messages == 0) {
        return InvalidConfigError(
            "max inflight messages must be greater than 0").ToError();
    }

    if (read_only_option == ReadOnlyOption::LeaseBased && !check_quorum) {
        return InvalidConfigError(
            "read_only_option == LeaseBased requires check_quorum == true").ToError();
    }

    if (max_uncommitted_size < max_size_per_message) {
        return InvalidConfigError(
            "max uncommitted size should greater than max_size_per_msg").ToError();
    }

    return {};
}

}
