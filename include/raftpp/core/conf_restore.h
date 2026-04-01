#pragma once

#include <cstdint>

#include "raftpp/core/error.h"
#include "types.h"

namespace raftpp {

class ProgressTracker;

[[nodiscard]] Result<void> Restore(
    ProgressTracker& tracker, uint64_t next_idx, const ConfState& cs
);

}  // namespace raftpp
