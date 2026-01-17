#pragma once

#include "raftpp/core/error.h"
#include "raftpp/core/progress_tracker.h"
#include "raftpp/core/raftpp.pb.h"

namespace raftpp {

[[nodiscard]] Result<void> Restore(
    ProgressTracker& tracker, uint64_t next_idx, const ConfState& cs
);

}
