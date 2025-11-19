#include "raftpp/raft_core.h"

namespace raftpp {

RaftCore::RaftCore(const Config& config, std::unique_ptr<Storage> store)
    : term_(0),
      vote_(0),
      id_(config.id),
      raft_log_(config, std::move(store)),
      max_inflight_(config.max_inflight_messages),
      max_message_size_(config.max_size_per_message),
      pending_request_snapshot_(INVALID_INDEX),
      state_(StateRole::Follower),
      promotable_(false),
      leader_id_(0),
      pending_conf_index_(0),
      read_only_(config.read_only_option),
      election_elapsed_(0),
      heartbeat_elapsed_(0),
      check_quorum_(config.check_quorum),
      pre_vote_(config.pre_vote),
      skip_broadcast_commit_(config.skip_broadcast_commit),
      batch_append_(config.batch_append),
      disable_proposal_forwarding_(config.disable_proposal_forwarding),
      heartbeat_timeout_(config.heartbeat_tick),
      election_timeout_(config.election_tick),
      randomized_election_timeout_(0),
      min_election_timeout_(0),
      max_election_timeout_(0),
      priority_(0),
      uncommitted_state_(
          UncommittedState{
              .max_uncommitted_size = config.max_uncommitted_size,
              .uncommitted_size = 0,
              .last_log_tail_index = 0
          }
      ),
      max_committed_size_per_ready(config.max_committed_size_per_ready) {}

}  // namespace raftpp
