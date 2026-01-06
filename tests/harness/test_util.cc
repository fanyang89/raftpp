#include "test_util.h"

#include "harness/network.h"
#include "raftpp/memory_storage.h"
#include "raftpp/raft.h"
#include "raftpp/raft_config.h"

namespace raftpp {

Network CreateTestNetwork(size_t num_peers) {
    return Network::CreateWithConfig(num_peers, DefaultConfig());
}

bool AllPeersHaveSameLeader(const Network& network, uint64_t leader_id) {
    for (const auto& [id, peer] : network.Peers()) {
        if (peer->HasRaft() && peer->lead() != leader_id) {
            return false;
        }
    }
    return true;
}

}  // namespace raftpp
