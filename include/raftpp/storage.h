#pragma once

#include <expected>
#include <mutex>

#include "raftpp/error.h"
#include "raftpp/raftpp.pb.h"

namespace raftpp {

struct RaftState {
    HardState hard_state;
    ConfState conf_state;
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
    virtual Result<RaftState, RaftError> InitialState() = 0;
    virtual Result<std::vector<Entry>, RaftError> Entries(
        uint64_t low, uint64_t high, std::optional<uint64_t> max_size, GetEntriesContext context
    ) = 0;
    virtual Result<uint64_t, RaftError> Term(uint64_t idx) = 0;
    virtual Result<uint64_t, RaftError> FirstIndex() = 0;
    virtual Result<uint64_t, RaftError> LastIndex() = 0;
    virtual Result<Snapshot, RaftError> GetSnapshot(uint64_t request_index, uint64_t to) = 0;
};

}  // namespace raftpp
