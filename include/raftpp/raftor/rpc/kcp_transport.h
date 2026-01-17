#pragma once

#include <ikcp.h>
#include <uv.h>

#include <absl/container/flat_hash_map.h>

#include "peer_manager.h"
#include "transport.h"

namespace raftpp::raftor::rpc {

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

/// Represents a KCP session with a specific peer
struct KcpSession {
    ikcpcb* kcp = nullptr;
    uint32_t conv = 0;
    uint64_t peer_id = 0;
    sockaddr_storage remote_addr{};
    socklen_t addr_len = 0;
    bool handshake_done = false;
    bool is_initiator = false;
    std::vector<uint8_t> recv_buf;
    std::chrono::steady_clock::time_point last_activity;
    void* transport = nullptr;

    ~KcpSession();
};

/// Address key for hash map
struct AddrKey {
    sockaddr_storage addr{};

    bool operator==(const AddrKey& other) const;
};

struct AddrKeyHash {
    size_t operator()(const AddrKey& k) const;
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
    uint32_t AllocateConvId();
    KcpSession* CreateSession(
        uint32_t conv, uint64_t peer_id, const sockaddr_storage& addr, socklen_t addr_len,
        bool is_initiator
    );
    void DestroySession(KcpSession* session);
    void InitiateHandshake(uint64_t peer_id);
    void HandleKcpHandshake(const char* data, size_t len, const sockaddr* addr, socklen_t addr_len);
    void SendAppHandshake(KcpSession* session);
    void OnAppHandshakeReceived(KcpSession* session, uint64_t remote_id);
    void ProcessUdpPacket(const char* data, size_t len, const sockaddr* addr, socklen_t addr_len);
    void TryReceive(KcpSession* session);
    /// Returns false if session was destroyed during processing
    bool ProcessReceivedData(KcpSession* session);
    void SendUdp(const void* data, size_t len, const sockaddr* addr, socklen_t addr_len);
    void UpdateAllKcpSessions();

    // Static callbacks for libuv and KCP
    static int KcpOutput(const char* buf, int len, ikcpcb* kcp, void* user);
    static void OnAlloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
    static void OnRecv(
        uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, const sockaddr* addr, unsigned flags
    );
    static void OnKcpUpdate(uv_timer_t* timer);
    static void OnReconnectTimer(uv_timer_t* timer);

    TransportConfig config_;
    KcpConfig kcp_config_;

    uv_loop_t loop_{};
    uv_udp_t udp_handle_{};
    uv_timer_t update_timer_{};
    uv_timer_t reconnect_timer_{};

    PeerManager peer_manager_;
    Map<uint64_t, KcpSession*> sessions_by_peer_;
    Map<uint32_t, KcpSession*> sessions_by_conv_;
    absl::flat_hash_map<AddrKey, KcpSession*, AddrKeyHash> sessions_by_addr_;
    Map<uint64_t, sockaddr_storage> peer_addresses_;

    uint32_t next_conv_id_ = 1;

    MessageCallback on_message_;
    ErrorCallback on_error_;

    bool running_ = false;
    bool stopped_ = false;
};

}  // namespace raftpp::raftor::rpc
