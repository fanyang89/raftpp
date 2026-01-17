#pragma once

#include <uv.h>

#include "raftor/rpc/peer_manager.h"
#include "raftor/rpc/transport.h"

namespace raftpp::rpc {

/// Connection context for both client and server connections
struct Connection {
    uv_tcp_t handle;
    uint64_t peer_id = 0;         // 0 until handshake completes
    bool is_outgoing = false;     // true for client connections
    bool handshake_done = false;  // true after handshake exchange
    std::vector<uint8_t> read_buf;
    void* transport = nullptr;  // Pointer to TcpTransport

    Connection() { handle.data = this; }
};

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
    void TryConnect(uint64_t peer_id);
    void CloseConnection(Connection* conn);
    void SendRaw(Connection* conn, std::vector<uint8_t> data);
    void SendHandshake(Connection* conn);
    void OnHandshakeReceived(Connection* conn, uint64_t remote_id);
    void ProcessReadBuffer(Connection* conn);

    // Static callbacks for libuv
    static void OnNewConnection(uv_stream_t* server, int status);
    static void OnConnect(uv_connect_t* req, int status);
    static void OnAlloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
    static void OnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
    static void OnReconnectTimer(uv_timer_t* timer);

    TransportConfig config_;
    uv_loop_t loop_{};
    uv_tcp_t server_{};
    uv_timer_t reconnect_timer_{};

    PeerManager peer_manager_;
    Map<uint64_t, Connection*> connections_;      // peer_id -> connection
    Map<uv_tcp_t*, Connection*> handle_to_conn_;  // handle -> connection

    MessageCallback on_message_;
    ErrorCallback on_error_;

    bool running_ = false;
    bool stopped_ = false;
};

}  // namespace raftpp::rpc
