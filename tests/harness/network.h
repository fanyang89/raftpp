#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "harness/interface.h"
#include "raftpp/core/error.h"
#include "raftpp/core/memory_storage.h"
#include "raftpp/core/raft_config.h"
#include "raftpp/core/types.h"

namespace raftpp {

/// A connection from one node to another.
/// Used by Network for determining drop rates on messages.
struct Connection {
    uint64_t from;
    uint64_t to;

    bool operator==(const Connection& other) const { return from == other.from && to == other.to; }
};

}  // namespace raftpp

template <>
struct std::hash<raftpp::Connection> {
    size_t operator()(const raftpp::Connection& c) const noexcept {
        return std::hash<uint64_t>{}(c.from) ^ (std::hash<uint64_t>{}(c.to) << 1);
    }
};

namespace raftpp {

/// A simulated network for testing.
///
/// You can use this to create a test network of Raft nodes.
/// No actual network calls are made.
class Network {
  public:
    Network();

    /// Get the default config for test networks.
    static Config DefaultConfig();

    /// Create a network with the given number of peers using default config.
    static Network Create(size_t num_peers);

    /// Create a network with the given peers.
    /// A nullptr means a new Raft will be created at that position.
    static Network Create(std::vector<std::unique_ptr<Interface>> peers);

    /// Create a network with explicit config.
    static Network CreateWithConfig(
        std::vector<std::unique_ptr<Interface>> peers, const Config& config
    );

    /// Ignore a given MessageType.
    void IgnoreMessageType(MessageType type);

    /// Filter out messages that should be dropped.
    std::vector<Message> Filter(std::vector<Message> msgs);

    /// Read all messages from all peers.
    std::vector<Message> ReadMessages();

    /// Send messages and process all responses recursively.
    void Send(std::vector<Message> msgs);

    /// Filter and then send messages.
    void FilterAndSend(std::vector<Message> msgs);

    /// Dispatch messages without gathering responses.
    Result<void> Dispatch(std::vector<Message> msgs);

    /// Set drop rate for messages from one node to another.
    /// perc=1.0 means 100% drop rate, 0.0 means 0% drop rate.
    void Drop(uint64_t from, uint64_t to, double perc);

    /// Cut communication between two nodes (100% drop both ways).
    void Cut(uint64_t one, uint64_t other);

    /// Isolate a node from all others.
    void Isolate(uint64_t id);

    /// Reset all drop/ignore rules.
    void Recover();

    /// Get peer by ID, returns nullptr if not found.
    Interface* GetPeer(uint64_t id);
    const Interface* GetPeer(uint64_t id) const;

    /// Get storage by ID, returns nullptr if not found.
    std::shared_ptr<MemoryStorage> GetStorage(uint64_t id);

    /// Get number of peers.
    size_t Size() const { return peers_.size(); }

    /// Access peers map directly (for iteration).
    std::unordered_map<uint64_t, Interface>& peers() { return peers_; }

    const std::unordered_map<uint64_t, Interface>& peers() const { return peers_; }

  private:
    std::unordered_map<uint64_t, Interface> peers_;
    std::unordered_map<uint64_t, std::shared_ptr<MemoryStorage>> storage_;
    std::unordered_map<Connection, double> drop_map_;
    std::unordered_map<MessageType, bool> ignore_map_;
    std::mt19937 rng_;
};

/// Convenience function to create a test network.
Network CreateTestNetwork(size_t num_peers);

/// Create a test network with explicit config.
Network CreateTestNetworkWithConfig(size_t num_peers, const Config& config);

}  // namespace raftpp
