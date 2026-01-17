#include "raftpp/rpc/tcp_transport.h"

#include <uv.h>

#include <cstring>

#include <spdlog/spdlog.h>

#include "raftpp/rpc/peer_manager.h"

namespace raftpp::rpc {

namespace {

/// Parse address string "host:port" into components
RpcResult<std::pair<std::string, int>> ParseAddress(const std::string& addr) {
    auto colon = addr.rfind(':');
    if (colon == std::string::npos) {
        return std::unexpected(RpcError::InvalidAddress("missing port in address: " + addr));
    }

    std::string host = addr.substr(0, colon);
    int port = 0;
    try {
        port = std::stoi(addr.substr(colon + 1));
    } catch (...) {
        return std::unexpected(RpcError::InvalidAddress("invalid port in address: " + addr));
    }

    if (port <= 0 || port > 65535) {
        return std::unexpected(RpcError::InvalidAddress("port out of range: " + addr));
    }

    return std::pair{host, port};
}

}  // namespace

/// Connection context for both client and server connections
struct Connection {
    uv_tcp_t handle;
    uint64_t peer_id = 0;         // 0 until handshake completes
    bool is_outgoing = false;     // true for client connections
    bool handshake_done = false;  // true after handshake exchange
    std::vector<uint8_t> read_buf;
    void* transport = nullptr;  // Pointer to TcpTransport::Impl

    Connection() { handle.data = this; }
};

struct TcpTransport::Impl {
    TransportConfig config;
    uv_loop_t loop;
    uv_tcp_t server;
    uv_timer_t reconnect_timer;

    PeerManager peer_manager;
    Map<uint64_t, Connection*> connections;      // peer_id -> connection
    Map<uv_tcp_t*, Connection*> handle_to_conn;  // handle -> connection

    MessageCallback on_message;
    ErrorCallback on_error;

    bool running = false;
    bool stopped = false;

    Impl(TransportConfig cfg) : config(std::move(cfg)) {
        uv_loop_init(&loop);
        server.data = this;
        reconnect_timer.data = this;
    }

    ~Impl() {
        Stop();
        uv_loop_close(&loop);
    }

    RpcResult<void> Start() {
        if (running) {
            return {};
        }

        // Parse listen address
        auto addr_result = ParseAddress(config.listen_addr);
        if (!addr_result) {
            return std::unexpected(addr_result.error());
        }
        auto [host, port] = *addr_result;

        // Initialize server
        uv_tcp_init(&loop, &server);

        sockaddr_in addr;
        uv_ip4_addr(host.c_str(), port, &addr);

        int r = uv_tcp_bind(&server, reinterpret_cast<const sockaddr*>(&addr), 0);
        if (r != 0) {
            return std::unexpected(
                RpcError::ConnectionFailed(std::string("bind failed: ") + uv_strerror(r))
            );
        }

        r = uv_listen(reinterpret_cast<uv_stream_t*>(&server), 128, OnNewConnection);
        if (r != 0) {
            return std::unexpected(
                RpcError::ConnectionFailed(std::string("listen failed: ") + uv_strerror(r))
            );
        }

        // Initialize reconnect timer
        uv_timer_init(&loop, &reconnect_timer);
        uv_timer_start(&reconnect_timer, OnReconnectTimer, 1000, 1000);

        running = true;
        stopped = false;

        SPDLOG_INFO("Transport started on {}", config.listen_addr);

        // Connect to existing peers
        for (auto peer_id : peer_manager.GetAllPeerIds()) {
            TryConnect(peer_id);
        }

        return {};
    }

    void Stop() {
        if (!running || stopped) {
            return;
        }
        stopped = true;

        // Close all connections
        for (auto& [_, conn] : connections) {
            CloseConnection(conn);
        }
        connections.clear();
        handle_to_conn.clear();

        // Stop timer
        uv_timer_stop(&reconnect_timer);
        uv_close(reinterpret_cast<uv_handle_t*>(&reconnect_timer), nullptr);

        // Close server
        uv_close(reinterpret_cast<uv_handle_t*>(&server), nullptr);

        // Run loop to process close callbacks
        while (uv_loop_alive(&loop)) {
            uv_run(&loop, UV_RUN_ONCE);
        }

        running = false;
        SPDLOG_INFO("Transport stopped");
    }

    void AddPeer(uint64_t id, const std::string& addr) {
        peer_manager.AddPeer(id, addr);
        if (running) {
            TryConnect(id);
        }
    }

    void RemovePeer(uint64_t id) {
        peer_manager.RemovePeer(id);
        if (auto it = connections.find(id); it != connections.end()) {
            CloseConnection(it->second);
            connections.erase(it);
        }
    }

    void Send(std::span<const Message> messages) {
        for (const auto& msg : messages) {
            uint64_t to = msg.to();
            auto it = connections.find(to);
            if (it == connections.end() || !it->second->handshake_done) {
                SPDLOG_DEBUG("Dropping message to {}: not connected", to);
                continue;
            }

            auto buf = Codec::Encode(msg);
            SendRaw(it->second, std::move(buf));
        }
    }

    void Poll(std::chrono::milliseconds timeout) {
        uv_run(&loop, UV_RUN_NOWAIT);

        // Also run with timeout if specified
        if (timeout.count() > 0) {
            // Create a timer for the timeout
            uv_timer_t timer;
            timer.data = &loop;
            uv_timer_init(&loop, &timer);
            uv_timer_start(
                &timer, [](uv_timer_t* t) { uv_stop(static_cast<uv_loop_t*>(t->data)); },
                timeout.count(), 0
            );

            uv_run(&loop, UV_RUN_DEFAULT);

            uv_timer_stop(&timer);
            uv_close(reinterpret_cast<uv_handle_t*>(&timer), nullptr);
            uv_run(&loop, UV_RUN_NOWAIT);  // Process close
        }
    }

    void Run() { uv_run(&loop, UV_RUN_DEFAULT); }

    void TryConnect(uint64_t peer_id) {
        auto* peer = peer_manager.GetPeer(peer_id);
        if (!peer || peer->state != PeerState::Disconnected) {
            return;
        }

        auto addr_result = ParseAddress(peer->addr);
        if (!addr_result) {
            SPDLOG_WARN("Invalid peer address for {}: {}", peer_id, peer->addr);
            return;
        }
        auto [host, port] = *addr_result;

        auto* conn = new Connection();
        conn->is_outgoing = true;
        conn->peer_id = peer_id;
        conn->transport = this;

        uv_tcp_init(&loop, &conn->handle);
        handle_to_conn[&conn->handle] = conn;

        auto* req = new uv_connect_t();
        req->data = conn;

        sockaddr_in addr;
        uv_ip4_addr(host.c_str(), port, &addr);

        peer_manager.UpdateState(peer_id, PeerState::Connecting);

        int r =
            uv_tcp_connect(req, &conn->handle, reinterpret_cast<const sockaddr*>(&addr), OnConnect);
        if (r != 0) {
            SPDLOG_WARN("Connect to {} failed: {}", peer_id, uv_strerror(r));
            delete req;
            delete conn;
            peer_manager.RecordFailure(peer_id);
            return;
        }

        SPDLOG_DEBUG("Connecting to peer {} at {}", peer_id, peer->addr);
    }

    void CloseConnection(Connection* conn) {
        if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&conn->handle))) {
            uv_close(reinterpret_cast<uv_handle_t*>(&conn->handle), [](uv_handle_t* h) {
                auto* c = static_cast<Connection*>(h->data);
                delete c;
            });
        }
        handle_to_conn.erase(&conn->handle);
    }

    void SendRaw(Connection* conn, std::vector<uint8_t> data) {
        auto* req = new uv_write_t();
        auto* buf_data = new std::vector<uint8_t>(std::move(data));
        req->data = buf_data;

        uv_buf_t buf = uv_buf_init(reinterpret_cast<char*>(buf_data->data()), buf_data->size());

        uv_write(
            req, reinterpret_cast<uv_stream_t*>(&conn->handle), &buf, 1,
            [](uv_write_t* req, int status) {
                auto* buf_data = static_cast<std::vector<uint8_t>*>(req->data);
                delete buf_data;
                delete req;

                if (status != 0) {
                    SPDLOG_DEBUG("Write failed: {}", uv_strerror(status));
                }
            }
        );
    }

    void SendHandshake(Connection* conn) {
        Handshake hs;
        hs.node_id = config.node_id;
        SendRaw(conn, hs.Encode());
    }

    void OnHandshakeReceived(Connection* conn, uint64_t remote_id) {
        if (conn->is_outgoing) {
            // For outgoing connections, we already know the peer_id
            if (conn->peer_id != remote_id) {
                SPDLOG_WARN("Peer ID mismatch: expected {}, got {}", conn->peer_id, remote_id);
                // Still accept if IDs don't match (peer might have restarted)
            }
        } else {
            // For incoming connections, we learn the peer_id from handshake
            conn->peer_id = remote_id;

            // Check for existing connection
            if (auto it = connections.find(remote_id); it != connections.end()) {
                // Close old connection
                CloseConnection(it->second);
                connections.erase(it);
            }
        }

        conn->handshake_done = true;
        connections[conn->peer_id] = conn;
        peer_manager.UpdateState(conn->peer_id, PeerState::Connected);

        SPDLOG_DEBUG("Handshake complete with peer {}", conn->peer_id);
    }

    void ProcessReadBuffer(Connection* conn) {
        while (true) {
            auto& buf = conn->read_buf;
            if (buf.empty()) {
                break;
            }

            if (!conn->handshake_done) {
                // Expecting handshake
                if (buf.size() < Handshake::kSize) {
                    break;  // Need more data
                }

                auto hs_result = Handshake::Decode(buf);
                if (!hs_result) {
                    SPDLOG_WARN("Invalid handshake: {}", hs_result.error().ToString());
                    CloseConnection(conn);
                    return;
                }

                OnHandshakeReceived(conn, hs_result->node_id);
                buf.erase(buf.begin(), buf.begin() + Handshake::kSize);

                // Send our handshake if this is incoming connection
                if (!conn->is_outgoing) {
                    SendHandshake(conn);
                }
                continue;
            }

            // Expecting message frame
            auto frame_result = Codec::FrameSize(buf, config.max_message_size);
            if (!frame_result) {
                SPDLOG_WARN("Invalid frame: {}", frame_result.error().ToString());
                CloseConnection(conn);
                return;
            }

            size_t frame_size = *frame_result;
            if (frame_size == 0 || buf.size() < frame_size) {
                break;  // Need more data
            }

            auto decode_result = Codec::Decode(buf, config.max_message_size);
            if (!decode_result) {
                SPDLOG_WARN("Decode failed: {}", decode_result.error().ToString());
                CloseConnection(conn);
                return;
            }

            auto [header, msg, consumed] = std::move(*decode_result);
            if (consumed == 0) {
                break;  // Should not happen after FrameSize check
            }

            buf.erase(buf.begin(), buf.begin() + consumed);
            peer_manager.RecordActivity(conn->peer_id);

            if (on_message) {
                on_message(std::move(msg));
            }
        }
    }

    // Static callbacks for libuv
    static void OnNewConnection(uv_stream_t* server, int status) {
        auto* impl = static_cast<Impl*>(server->data);
        if (status < 0) {
            SPDLOG_WARN("Accept error: {}", uv_strerror(status));
            return;
        }

        auto* conn = new Connection();
        conn->is_outgoing = false;
        conn->transport = impl;

        uv_tcp_init(&impl->loop, &conn->handle);
        impl->handle_to_conn[&conn->handle] = conn;

        if (uv_accept(server, reinterpret_cast<uv_stream_t*>(&conn->handle)) == 0) {
            uv_read_start(reinterpret_cast<uv_stream_t*>(&conn->handle), OnAlloc, OnRead);
            SPDLOG_DEBUG("Accepted incoming connection");
        } else {
            impl->CloseConnection(conn);
        }
    }

    static void OnConnect(uv_connect_t* req, int status) {
        auto* conn = static_cast<Connection*>(req->data);
        auto* impl = static_cast<Impl*>(conn->transport);
        delete req;

        if (status < 0) {
            SPDLOG_WARN("Connect to {} failed: {}", conn->peer_id, uv_strerror(status));
            impl->peer_manager.RecordFailure(conn->peer_id);
            impl->handle_to_conn.erase(&conn->handle);

            if (impl->on_error) {
                impl->on_error(conn->peer_id, uv_strerror(status));
            }

            impl->CloseConnection(conn);
            return;
        }

        SPDLOG_DEBUG("Connected to peer {}", conn->peer_id);

        // Start reading
        uv_read_start(reinterpret_cast<uv_stream_t*>(&conn->handle), OnAlloc, OnRead);

        // Send handshake
        impl->SendHandshake(conn);
    }

    static void OnAlloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
        buf->base = new char[suggested_size];
        buf->len = suggested_size;
    }

    static void OnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
        auto* conn = static_cast<Connection*>(stream->data);
        auto* impl = static_cast<Impl*>(conn->transport);

        if (nread < 0) {
            if (nread != UV_EOF) {
                SPDLOG_DEBUG("Read error: {}", uv_strerror(nread));
            }

            if (conn->peer_id != 0) {
                impl->peer_manager.RecordFailure(conn->peer_id);
                impl->connections.erase(conn->peer_id);

                if (impl->on_error) {
                    impl->on_error(
                        conn->peer_id, nread == UV_EOF ? "connection closed" : uv_strerror(nread)
                    );
                }
            }

            impl->handle_to_conn.erase(&conn->handle);
            impl->CloseConnection(conn);
            delete[] buf->base;
            return;
        }

        if (nread > 0) {
            conn->read_buf.insert(conn->read_buf.end(), buf->base, buf->base + nread);
            impl->ProcessReadBuffer(conn);
        }

        delete[] buf->base;
    }

    static void OnReconnectTimer(uv_timer_t* timer) {
        auto* impl = static_cast<Impl*>(timer->data);
        if (impl->stopped) {
            return;
        }

        for (auto peer_id : impl->peer_manager.GetPeersToReconnect()) {
            impl->TryConnect(peer_id);
        }
    }
};

TcpTransport::TcpTransport(TransportConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

TcpTransport::~TcpTransport() = default;

RpcResult<void> TcpTransport::Start() {
    return impl_->Start();
}

void TcpTransport::Stop() {
    impl_->Stop();
}

void TcpTransport::AddPeer(uint64_t id, const std::string& addr) {
    impl_->AddPeer(id, addr);
}

void TcpTransport::RemovePeer(uint64_t id) {
    impl_->RemovePeer(id);
}

void TcpTransport::Send(std::span<const Message> messages) {
    impl_->Send(messages);
}

void TcpTransport::SetMessageCallback(MessageCallback cb) {
    impl_->on_message = std::move(cb);
}

void TcpTransport::SetErrorCallback(ErrorCallback cb) {
    impl_->on_error = std::move(cb);
}

void TcpTransport::Poll(std::chrono::milliseconds timeout) {
    impl_->Poll(timeout);
}

void TcpTransport::Run() {
    impl_->Run();
}

}  // namespace raftpp::rpc
