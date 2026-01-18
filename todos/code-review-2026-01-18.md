# Code review todos

## Findings (ordered by severity)

- **Critical**: `raftpp::Config` has multiple fields without defaults and `RaftorConfig::ToRaftConfig()` uses default-initialization (`Config cfg;`), leaving fields uninitialized and causing UB/random behavior (e.g., `min_election_tick`, `skip_broadcast_commit`, `max_apply_unpersisted_log_limit`, etc.).
  - Files: `include/raftpp/core/raft_config.h`, `lib/raftor/raftor.cc`

- **Critical**: Error reporting collapses many distinct failures into `ProposalDropped` (config validation, storage append/sync, transport start, WAL open). `ProposalTracker::FailAll` also discards the incoming error. This hides root causes and makes diagnosis/recovery difficult.
  - Files: `lib/raftor/raftor.cc`, `lib/raftor/ready_processor.cc`, `lib/raftor/proposal_tracker.cc`

- **High**: API claims custom `Storage` injection, but implementation requires `WALStorage` via `dynamic_pointer_cast`, causing runtime failure for other storage backends and violating the abstraction.
  - Files: `include/raftpp/raftor/raftor.h`, `lib/raftor/raftor.cc`

- **Medium**: `Start()` documentation says the event loop is running, but implementation only starts transport and sets flags. Non-blocking usage (`Start()` + `Propose`) will not process ticks/ready unless caller manually calls `Poll()`.
  - Files: `include/raftpp/raftor/raftor.h`, `lib/raftor/raftor.cc`

- **Medium**: Config change handling for `AddNode` only logs and does not update transport when changes are applied from log (unless the user separately calls `AddNode`). Cluster membership may diverge across nodes.
  - Files: `lib/raftor/ready_processor.cc`

- **Medium**: Thread-safety is unclear for `GetStatus()`, `IsLeader()`, and `GetLeaderId()`. These can be called from non-event-loop threads but touch shared state without a clear concurrency model, risking races.
  - Files: `lib/raftor/raftor.cc`

## Additional risk

- `ApplyCommittedEntries()` logs state machine errors but continues and does not surface failure; this conflicts with `ApplyEntry()` error returns and may mislead callers about commit success.
  - Files: `lib/raftor/ready_processor.cc`
