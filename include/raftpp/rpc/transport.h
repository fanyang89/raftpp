#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

#include "raftpp/rpc/codec.h"

namespace raftpp::rpc {

/// Configuration for transport layer
struct TransportConfig {
    /// Address to listen on (e.g., "0.0.0.0:9000")
    std::string listen_addr;

    /// This node's ID
    uint64_t node_id = 0;

    /// Maximum message size in bytes (default: 64 MB)
    size_t max_message_size = 64 * 1024 * 1024;

    /// Timeout for establishing connections
    std::chrono::milliseconds connect_timeout{5000};

    /// Base interval for reconnection attempts
    std::chrono::milliseconds reconnect_interval{1000};
};

/// Callback for received messages
using MessageCallback = std::function<void(Message)>;

/// Callback for peer errors
using ErrorCallback = std::function<void(uint64_t peer_id, std::string error)>;

/// Abstract transport interface for Raft message passing
///
/// The transport layer is responsible for:
/// - Listening for incoming connections from peers
/// - Maintaining connections to peers
/// - Sending and receiving Raft messages
/// - Automatic reconnection on failure
class Transport {
  public:
    virtual ~Transport() = default;

    /// Start the transport (begin listening and connecting)
    virtual Result<void> Start() = 0;

    /// Stop the transport (close all connections)
    virtual void Stop() = 0;

    /// Add a peer to connect to
    virtual void AddPeer(uint64_t id, const std::string& addr) = 0;

    /// Remove a peer
    virtual void RemovePeer(uint64_t id) = 0;

    /// Send messages to peers (fire-and-forget)
    ///
    /// Messages are routed based on the `to` field in each message.
    /// Messages to unknown peers are silently dropped.
    virtual void Send(std::span<const Message> messages) = 0;

    /// Set callback for received messages
    virtual void SetMessageCallback(MessageCallback cb) = 0;

    /// Set callback for peer errors
    virtual void SetErrorCallback(ErrorCallback cb) = 0;

    /// Poll for events with timeout
    ///
    /// Processes network I/O and invokes callbacks for received messages.
    /// Returns when timeout expires or there are no more events.
    virtual void Poll(std::chrono::milliseconds timeout) = 0;

    /// Run the event loop (blocking)
    virtual void Run() = 0;
};

}  // namespace raftpp::rpc
