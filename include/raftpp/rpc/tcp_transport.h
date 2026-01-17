#pragma once

#include <memory>

#include "raftpp/rpc/transport.h"

namespace raftpp::rpc {

/// TCP-based transport implementation using libuv
///
/// This transport uses libuv for asynchronous I/O with a single-threaded
/// event loop model. It handles:
/// - TCP server for accepting incoming connections
/// - TCP client connections to peers
/// - Automatic reconnection with exponential backoff
/// - Message framing using Codec
/// - Handshake protocol for peer identification
class TcpTransport : public Transport {
  public:
    explicit TcpTransport(TransportConfig config);
    ~TcpTransport() override;

    // Non-copyable, non-movable (due to libuv handles)
    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;
    TcpTransport(TcpTransport&&) = delete;
    TcpTransport& operator=(TcpTransport&&) = delete;

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
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace raftpp::rpc
