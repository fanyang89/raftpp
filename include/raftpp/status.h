#pragma once

#include <cstdint>

namespace raftpp {

struct Status {
    /// The ID of the current node.
    uint64_t id;

    /// The hardstate of the raft, representing voted state.
    HardState hs;

    /// The softstate of the raft, representing proposed state.
    SoftState ss;

    /// The index of the last entry to have been applied.
    uint64_t applied;

    /// The progress towards catching up and applying logs.
    std::optional<ProgressTracker> progress;
}

}
