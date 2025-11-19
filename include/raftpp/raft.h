#pragma once

#include "raftpp/raft_core.h"
#include "raftpp/raft_log.h"
#include "raftpp/raftpp.pb.h"
#include "raftpp/readonly.h"
#include "raftpp/tracker.h"

namespace raftpp {

class Raft : public RaftCore {
  public:
    Raft(const Config& config, std::unique_ptr<Storage> store);

    ConfState PostConfChange();
    void LoadState(const HardState& hard_state);

    ProgressTracker& progress_tracker();
    const ProgressTracker& progress_tracker() const;

  private:
    void CommitApplyInternal(uint64_t applied, bool skip_check);
    void AbortLeaderTransfer();
    void Reset(uint64_t term);
    void BecomeFollower(uint64_t term, uint64_t leader_id);
    void ResetRandomizedElectionTimeout();

    ProgressTracker progress_tracker_;
    std::vector<Message> messages_;
    Config config_;
};

}  // namespace raftpp
