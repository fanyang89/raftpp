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

    ProgressTracker& progress_tracker();
    const ProgressTracker& progress_tracker() const;

  private:
    ProgressTracker progress_tracker_;
    std::vector<Message> messages_;
    Config config_;
};

}  // namespace raftpp
