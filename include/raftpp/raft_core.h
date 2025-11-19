#pragma once

#include <optional>
#include <span>

#include "progress.h"
#include "raftpp/config.h"
#include "raftpp/raft_log.h"
#include "raftpp/raftpp.pb.h"
#include "raftpp/storage.h"

namespace raftpp {

enum class StateRole : uint8_t {
    /// The node is a follower of the leader.
    Follower,
    /// The node could become a leader.
    Candidate,
    /// The node is a leader.
    Leader,
    /// The node could become a candidate, if `prevote` is enabled.
    PreCandidate,
};

struct SoftState {
    uint64_t leader_id;
    StateRole raft_state;
};

struct UncommittedState {
    size_t max_uncommitted_size;
    size_t uncommitted_size;
    uint64_t last_log_tail_index;

    bool IsNoLimit() const;
    bool MaybeIncreaseUncommittedSize(std::span<const Entry> entries);
    bool MaybeReduceUncommittedSize(std::span<const Entry> entries);
};

class RaftCore {
  public:
    RaftCore(const Config& config, std::unique_ptr<Storage> store);

    bool TryBatching(uint64_t to, std::vector<Message>& messages, Progress& pr, const std::vector<Entry>& entries
    ) const;
    void PrepareSendEntries(Message& message, Progress& pr, uint64_t term, const std::vector<Entry>& entries) const;
    bool MaybeSendAppend(uint64_t to, Progress& pr, bool allow_empty, std::vector<Message>& messages);
    void SendAppend(uint64_t to, Progress& pr, std::vector<Message>& messages);
    void SendAppendAggressively(uint64_t to, Progress& pr, std::vector<Message>& messages);
    void Send(Message& m, std::vector<Message>& messages) const;

  protected:
    bool PrepareSendSnapshot(Message& m, Progress& pr, uint64_t to);

    uint64_t term_;
    uint64_t vote_;
    uint64_t id_;
    std::vector<ReadState> read_states_;
    RaftLog raft_log_;
    size_t max_inflight_;
    uint64_t max_message_size_;
    uint64_t pending_request_snapshot_;
    StateRole state_;
    bool promotable_;
    uint64_t leader_id_;
    std::optional<uint64_t> lead_transferee_;
    uint64_t pending_conf_index_;
    ReadOnly read_only_;
    size_t election_elapsed_;
    size_t heartbeat_elapsed_;
    bool check_quorum_;
    bool pre_vote_;
    bool skip_broadcast_commit_;
    bool batch_append_;
    bool disable_proposal_forwarding_;

    size_t heartbeat_timeout_;
    size_t election_timeout_;

    // randomized_election_timeout is a random number between
    // [min_election_timeout, max_election_timeout - 1]. It gets reset
    // when raft changes its state to follower or candidate.
    size_t randomized_election_timeout_;
    size_t min_election_timeout_;
    size_t max_election_timeout_;

    /// The election priority of this node.
    int64_t priority_;

    /// Track uncommitted log entry on this node.
    UncommittedState uncommitted_state_;

    /// Max size per committed entries in a `Read`.
    uint64_t max_committed_size_per_ready_;
};

}  // namespace raftpp
