#pragma once

#include "raftpp/error.h"
#include "raftpp/raftpp.pb.h"
#include "raftpp/tracker.h"

namespace raftpp {

Result<void> Restore(ProgressTracker& tracker, uint64_t next_idx, const ConfState& cs);

}
