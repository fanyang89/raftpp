#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "error.h"
#include "raftpp/core/capnp_util.h"
#include "types.h"

namespace raftpp {

struct RaftState {
    HardState hard_state;
    ConfState conf_state;

    // Default constructor initializes empty messages
    RaftState()
        : hard_state(capnp_util::make<msg::HardState>()),
          conf_state(capnp_util::make<msg::ConfState>()) {}

    // Moveable but not copyable to avoid large copies
    RaftState(const RaftState&) = delete;
    RaftState& operator=(const RaftState&) = delete;
    RaftState(RaftState&&) noexcept = default;
    RaftState& operator=(RaftState&&) noexcept = default;

    // Deep clone
    [[nodiscard]] RaftState clone() const {
        RaftState rs;
        rs.hard_state = CloneHardState(hard_state);
        rs.conf_state = CloneConfState(conf_state);
        return rs;
    }
};

enum class GetEntriesFor {
    // for sending entries to followers
    SendAppend,
    // for getting committed entries in a ready
    GenReady,
    // for getting entries to check pending conf when transferring leader
    TransferLeader,
    // for getting entries to check pending conf when forwarding commit index by vote messages
    CommitByVote,
    // It's not called by the raft itself
    Empty,
};

union GetEntriesForPayload {
    struct Empty {
        bool can_async;
    };

    Empty empty;

    struct SendAppend {
        /// the peer id to which the entries are going to send
        uint64_t to;
        /// the term when the request is issued
        uint64_t term;
        /// whether to exhaust all the entries
        bool aggressively;
    };

    SendAppend send_append;
};

struct GetEntriesContext {
    GetEntriesFor what;
    GetEntriesForPayload payload;

    [[nodiscard]] bool CanAsync() const;

    static GetEntriesContext Empty(bool can_async);
};

class Storage {
  public:
    virtual ~Storage();
    [[nodiscard]] virtual Result<RaftState> InitialState() = 0;
    [[nodiscard]] virtual Result<std::vector<Entry>> Entries(
        uint64_t low, uint64_t high, std::optional<uint64_t> max_size, GetEntriesContext context
    ) = 0;
    [[nodiscard]] virtual Result<uint64_t> Term(uint64_t idx) = 0;
    [[nodiscard]] virtual Result<uint64_t> FirstIndex() = 0;
    [[nodiscard]] virtual Result<uint64_t> LastIndex() = 0;
    [[nodiscard]] virtual Result<Snapshot> GetSnapshot(uint64_t request_index, uint64_t to) = 0;
};

}  // namespace raftpp
