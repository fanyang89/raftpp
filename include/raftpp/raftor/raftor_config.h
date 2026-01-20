#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "raftpp/core/error.h"
#include "raftpp/core/raft_config.h"

namespace raftpp::raftor {

/// Peer node configuration
struct PeerConfig {
    /// Unique ID of the peer node
    uint64_t id = 0;

    /// Network address (e.g., "192.168.1.1:9000" or "hostname:port")
    std::string addr;
};

/// Complete Raftor configuration
struct RaftorConfig {
    /// This node's ID (must be unique in the cluster, must not be zero)
    uint64_t node_id = 0;

    /// Address to listen on for incoming connections (e.g., "0.0.0.0:9000")
    std::string listen_addr;

    /// Initial cluster configuration (for bootstrap)
    /// Should include this node and all other initial peers
    std::vector<PeerConfig> initial_peers;

    /// Base directory for all storage files (WAL, snapshots)
    std::filesystem::path data_dir;

    /// Election timeout in ticks (default: 10)
    /// Actual timeout is election_tick * tick_interval
    size_t election_tick = 10;

    /// Heartbeat interval in ticks (default: 2)
    /// Actual interval is heartbeat_tick * tick_interval
    size_t heartbeat_tick = 2;

    /// Maximum size of a single message in bytes (default: 1MB)
    uint64_t max_size_per_message = 1024 * 1024;

    /// Maximum number of in-flight append messages per peer (default: 256)
    size_t max_inflight_messages = 256;

    /// Whether to use pre-vote (recommended: true)
    /// Pre-vote prevents disruption from partitioned nodes
    bool pre_vote = true;

    /// Whether leader should check quorum periodically (recommended: true)
    /// Required for leader lease-based reads
    bool check_quorum = true;

    /// Read consistency mode (default: Safe)
    ReadOnlyOption read_only_option = ReadOnlyOption::Safe;

    /// Tick interval - how often to advance Raft state (default: 100ms)
    std::chrono::milliseconds tick_interval{100};

    /// Network connection timeout (default: 5s)
    std::chrono::milliseconds connect_timeout{5000};

    /// Validate the configuration
    /// @return void on success, or error describing what's invalid
    [[nodiscard]] Result<void> Validate() const;

    /// Convert to core raftpp::Config for RawNode
    [[nodiscard]] raftpp::Config ToRaftConfig() const;
};

}  // namespace raftpp::raftor
