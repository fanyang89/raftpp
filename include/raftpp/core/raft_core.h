#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <nonstd/span.hpp>

#include "raft_log.h"
#include "read_only.h"
#include "types.h"

namespace raftpp {

class Progress;
class Storage;
struct Config;

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

constexpr std::string_view format_as(StateRole role) {
    switch (role) {
        case StateRole::Follower:
            return "Follower";
        case StateRole::Candidate:
            return "Candidate";
        case StateRole::Leader:
            return "Leader";
        case StateRole::PreCandidate:
            return "PreCandidate";
    }
    return "Unknown";
}

struct SoftState {
    uint64_t leader_id;
    StateRole raft_state;

    bool operator==(const SoftState& other) const {
        return leader_id == other.leader_id && raft_state == other.raft_state;
    }

    bool operator!=(const SoftState& other) const { return !(*this == other); }
};

struct UncommittedState {
    size_t max_uncommitted_size;
    size_t uncommitted_size;
    uint64_t last_log_tail_index;

    [[nodiscard]] bool IsNoLimit() const;
    [[nodiscard]] bool MaybeIncreaseUncommittedSize(nonstd::span<const Entry> entries);
    [[nodiscard]] bool MaybeReduceUncommittedSize(nonstd::span<const Entry> entries);
};

class RaftCore {
  public:
    RaftCore(const Config& config, const std::shared_ptr<Storage>& store);

    [[nodiscard]] bool TryBatching(
        uint64_t to, std::vector<Message>& messages, Progress& pr, const std::vector<Entry>& entries
    ) const;
    void PrepareSendEntries(
        Message& message, Progress& pr, uint64_t term, const std::vector<Entry>& entries
    ) const;
    [[nodiscard]] bool MaybeSendAppend(
        uint64_t to, Progress& pr, bool allow_empty, std::vector<Message>& messages
    );
    void SendAppend(uint64_t to, Progress& pr, std::vector<Message>& messages);
    void SendAppendAggressively(uint64_t to, Progress& pr, std::vector<Message>& messages);
    void Send(Message& m, std::vector<Message>& messages) const;

  protected:
    friend class Interface;  // For testing access to protected members

    [[nodiscard]] bool PrepareSendSnapshot(Message& m, Progress& pr, uint64_t to);

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
