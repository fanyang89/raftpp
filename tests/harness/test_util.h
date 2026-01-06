#pragma once

#include "harness/network.h"
#include "raftpp/raft_config.h"

namespace raftpp {

/// Create a network with specified number of peers using default config.
inline Network CreateTestNetwork(size_t num_peers) {
    return Network::CreateWithConfig(num_peers, DefaultConfig());
}

}  // namespace raftpp
