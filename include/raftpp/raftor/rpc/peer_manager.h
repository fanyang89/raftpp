#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "../../core/primitives.h"

namespace raftpp::raftor::rpc {

/// Connection state of a peer
enum class PeerState {
    /// Not connected, may need reconnection
    Disconnected,
    /// Connection attempt in progress
    Connecting,
    /// Successfully connected and ready for communication
    Connected,
};

/// Information about a peer node
struct PeerInfo {
    /// Unique node ID
    uint64_t id = 0;

    /// Network address (e.g., "192.168.1.1:9000")
    std::string addr;

    /// Current connection state
    PeerState state = PeerState::Disconnected;

    /// Time of last activity (send or receive)
    std::chrono::steady_clock::time_point last_activity;

    /// Number of consecutive connection failures (for exponential backoff)
    uint32_t failure_count = 0;

    /// Time when reconnection should be attempted
    std::chrono::steady_clock::time_point reconnect_after;
};

/// Manages peer information and connection states
class PeerManager {
  public:
    /// Add a peer with the given ID and address
    void AddPeer(uint64_t id, std::string addr);

    /// Remove a peer by ID
    void RemovePeer(uint64_t id);

    /// Get peer info by ID, returns nullptr if not found
    PeerInfo* GetPeer(uint64_t id);
    const PeerInfo* GetPeer(uint64_t id) const;

    /// Get all peer IDs
    std::vector<uint64_t> GetAllPeerIds() const;

    /// Get IDs of peers that need reconnection
    std::vector<uint64_t> GetPeersToReconnect() const;

    /// Update peer connection state
    void UpdateState(uint64_t id, PeerState state);

    /// Record connection failure and schedule reconnection with backoff
    void RecordFailure(uint64_t id);

    /// Record successful activity
    void RecordActivity(uint64_t id);

    /// Check if peer exists
    bool HasPeer(uint64_t id) const;

    /// Get number of peers
    size_t Size() const { return peers_.size(); }

    /// Get number of connected peers
    size_t ConnectedCount() const;

  private:
    Map<uint64_t, PeerInfo> peers_;

    /// Calculate reconnection delay based on failure count (exponential backoff)
    static std::chrono::milliseconds CalculateBackoff(uint32_t failure_count);
};

}  // namespace raftpp::raftor::rpc
