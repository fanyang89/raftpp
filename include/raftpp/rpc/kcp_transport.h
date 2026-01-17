#pragma once

#include <memory>

#include "raftpp/rpc/transport.h"

namespace raftpp::rpc {

/// KCP-specific configuration options
struct KcpConfig {
    /// KCP nodelay mode (0: off, 1: on) - enables fast mode
    int nodelay = 1;

    /// KCP internal update interval in milliseconds
    int interval = 10;

    /// Fast resend trigger (0: off, 2: 2 ACK spans trigger resend)
    int resend = 2;

    /// Disable congestion control (0: normal, 1: disable)
    int nc = 1;

    /// Send window size
    int snd_wnd = 128;

    /// Receive window size
    int rcv_wnd = 128;

    /// MTU size (default: 1400, leaving room for IP/UDP headers)
    int mtu = 1400;

    /// Session timeout in milliseconds (detect dead connections)
    uint32_t session_timeout_ms = 30000;
};

/// KCP-based transport implementation using libuv UDP
///
/// This transport uses KCP for reliable, low-latency message delivery over UDP.
/// It provides virtual connections with per-peer KCP sessions and handles:
/// - UDP socket I/O via libuv
/// - Per-peer KCP session management with unique conversation IDs
/// - Handshake protocol for peer identification
/// - Periodic KCP update() calls via timer
/// - Message framing using existing Codec
class KcpTransport : public Transport {
  public:
    explicit KcpTransport(TransportConfig config, KcpConfig kcp_config = {});
    ~KcpTransport() override;

    // Non-copyable, non-movable
    KcpTransport(const KcpTransport&) = delete;
    KcpTransport& operator=(const KcpTransport&) = delete;
    KcpTransport(KcpTransport&&) = delete;
    KcpTransport& operator=(KcpTransport&&) = delete;

    // Transport interface
    RpcResult<void> Start() override;
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
