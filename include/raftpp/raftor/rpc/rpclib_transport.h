#pragma once

#include <mutex>
#include <queue>

#include <rpc/client.h>
#include <rpc/server.h>

#include "raftpp/raftor/rpc/peer_manager.h"
#include "raftpp/raftor/rpc/transport.h"

namespace raftpp::raftor::rpc {

/// RPC transport implementation using rpclib (msgpack-RPC)
///
/// This transport uses rpclib for asynchronous RPC communication.
/// - Server runs with async_run(1) for single-threaded background processing
/// - Clients use send() for fire-and-forget notifications
/// - Messages are serialized as protobuf binary blobs
class RpclibTransport : public Transport {
  public:
    explicit RpclibTransport(TransportConfig config);
    ~RpclibTransport() override;

    // Non-copyable, non-movable
    RpclibTransport(const RpclibTransport&) = delete;
    RpclibTransport& operator=(const RpclibTransport&) = delete;
    RpclibTransport(RpclibTransport&&) = delete;
    RpclibTransport& operator=(RpclibTransport&&) = delete;

    // Transport interface
    Result<void> Start() override;
    void Stop() override;
    void AddPeer(uint64_t id, const std::string& addr) override;
    void RemovePeer(uint64_t id) override;
    void Send(std::span<const Message> messages) override;
    void SetMessageCallback(MessageCallback cb) override;
    void SetErrorCallback(ErrorCallback cb) override;
    void Poll(std::chrono::milliseconds timeout) override;
    void Run() override;

  private:
    void TryConnect(uint64_t peer_id);
    void OnRaftMessage(uint64_t from, const std::string& data);

    TransportConfig config_;
    std::unique_ptr<::rpc::server> server_;
    Map<uint64_t, std::unique_ptr<::rpc::client>> clients_;

    PeerManager peer_manager_;

    // Incoming message queue (filled by server, consumed by Poll)
    std::mutex queue_mutex_;
    std::queue<Message> incoming_queue_;

    MessageCallback on_message_;
    ErrorCallback on_error_;

    bool running_ = false;
    bool stopped_ = false;
};

}  // namespace raftpp::raftor::rpc
