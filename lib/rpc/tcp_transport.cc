#include "raftpp/rpc/tcp_transport.h"

#include <cstring>

#include <spdlog/spdlog.h>

namespace raftpp::rpc {

TcpTransport::TcpTransport(TransportConfig config) : config_(std::move(config)) {
    uv_loop_init(&loop_);
    server_.data = this;
    reconnect_timer_.data = this;
}

TcpTransport::~TcpTransport() {
    Stop();
    uv_loop_close(&loop_);
}

Result<void> TcpTransport::Start() {
    if (running_) {
        return {};
    }

    // Parse listen address
    auto addr_result = ParseAddress(config_.listen_addr);
    if (!addr_result) {
        return std::unexpected(addr_result.error());
    }
    auto [host, port] = *addr_result;

    // Initialize server
    uv_tcp_init(&loop_, &server_);

    sockaddr_in addr;
    uv_ip4_addr(host.c_str(), port, &addr);

    int r = uv_tcp_bind(&server_, reinterpret_cast<const sockaddr*>(&addr), 0);
    if (r != 0) {
        return RaftError(RpcErrorCode::BindFailed);
    }

    r = uv_listen(reinterpret_cast<uv_stream_t*>(&server_), 128, OnNewConnection);
    if (r != 0) {
        return RaftError(RpcErrorCode::ListenFailed);
    }

    // Initialize reconnect timer
    uv_timer_init(&loop_, &reconnect_timer_);
    uv_timer_start(&reconnect_timer_, OnReconnectTimer, 1000, 1000);

    running_ = true;
    stopped_ = false;

    SPDLOG_INFO("Transport started on {}", config_.listen_addr);

    // Connect to existing peers
    for (auto peer_id : peer_manager_.GetAllPeerIds()) {
        TryConnect(peer_id);
    }

    return {};
}

void TcpTransport::Stop() {
    if (!running_ || stopped_) {
        return;
    }
    stopped_ = true;

    // Close all connections
    for (auto& [_, conn] : connections_) {
        CloseConnection(conn);
    }
    connections_.clear();
    handle_to_conn_.clear();

    // Stop timer
    uv_timer_stop(&reconnect_timer_);
    uv_close(reinterpret_cast<uv_handle_t*>(&reconnect_timer_), nullptr);

    // Close server
    uv_close(reinterpret_cast<uv_handle_t*>(&server_), nullptr);

    // Run loop to process close callbacks
    while (uv_loop_alive(&loop_)) {
        uv_run(&loop_, UV_RUN_ONCE);
    }

    running_ = false;
    SPDLOG_INFO("Transport stopped");
}

void TcpTransport::AddPeer(uint64_t id, const std::string& addr) {
    peer_manager_.AddPeer(id, addr);
    if (running_) {
        TryConnect(id);
    }
}

void TcpTransport::RemovePeer(uint64_t id) {
    peer_manager_.RemovePeer(id);
    if (auto it = connections_.find(id); it != connections_.end()) {
        CloseConnection(it->second);
        connections_.erase(it);
    }
}

void TcpTransport::Send(std::span<const Message> messages) {
    for (const auto& msg : messages) {
        uint64_t to = msg.to();
        auto it = connections_.find(to);
        if (it == connections_.end() || !it->second->handshake_done) {
            SPDLOG_DEBUG("Dropping message to {}: not connected", to);
            continue;
        }

        auto buf = Codec::Encode(msg);
        SendRaw(it->second, std::move(buf));
    }
}

void TcpTransport::SetMessageCallback(MessageCallback cb) {
    on_message_ = std::move(cb);
}

void TcpTransport::SetErrorCallback(ErrorCallback cb) {
    on_error_ = std::move(cb);
}

void TcpTransport::Poll(std::chrono::milliseconds timeout) {
    uv_run(&loop_, UV_RUN_NOWAIT);

    // Also run with timeout if specified
    if (timeout.count() > 0) {
        // Create a timer for the timeout
        uv_timer_t timer;
        timer.data = &loop_;
        uv_timer_init(&loop_, &timer);
        uv_timer_start(
            &timer, [](uv_timer_t* t) { uv_stop(static_cast<uv_loop_t*>(t->data)); },
            timeout.count(), 0
        );

        uv_run(&loop_, UV_RUN_DEFAULT);

        uv_timer_stop(&timer);
        uv_close(reinterpret_cast<uv_handle_t*>(&timer), nullptr);
        uv_run(&loop_, UV_RUN_NOWAIT);  // Process close
    }
}

void TcpTransport::Run() {
    uv_run(&loop_, UV_RUN_DEFAULT);
}

void TcpTransport::TryConnect(uint64_t peer_id) {
    auto* peer = peer_manager_.GetPeer(peer_id);
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

    uv_tcp_init(&loop_, &conn->handle);
    handle_to_conn_[&conn->handle] = conn;

    auto* req = new uv_connect_t();
    req->data = conn;

    sockaddr_in addr;
    uv_ip4_addr(host.c_str(), port, &addr);

    peer_manager_.UpdateState(peer_id, PeerState::Connecting);

    int r = uv_tcp_connect(req, &conn->handle, reinterpret_cast<const sockaddr*>(&addr), OnConnect);
    if (r != 0) {
        SPDLOG_WARN("Connect to {} failed: {}", peer_id, uv_strerror(r));
        delete req;
        delete conn;
        peer_manager_.RecordFailure(peer_id);
        return;
    }

    SPDLOG_DEBUG("Connecting to peer {} at {}", peer_id, peer->addr);
}

void TcpTransport::CloseConnection(Connection* conn) {
    if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&conn->handle))) {
        uv_close(reinterpret_cast<uv_handle_t*>(&conn->handle), [](uv_handle_t* h) {
            auto* c = static_cast<Connection*>(h->data);
            delete c;
        });
    }
    handle_to_conn_.erase(&conn->handle);
}

void TcpTransport::SendRaw(Connection* conn, std::vector<uint8_t> data) {
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

void TcpTransport::SendHandshake(Connection* conn) {
    Handshake hs;
    hs.node_id = config_.node_id;
    SendRaw(conn, hs.Encode());
}

void TcpTransport::OnHandshakeReceived(Connection* conn, uint64_t remote_id) {
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
        if (auto it = connections_.find(remote_id); it != connections_.end()) {
            // Close old connection
            CloseConnection(it->second);
            connections_.erase(it);
        }
    }

    conn->handshake_done = true;
    connections_[conn->peer_id] = conn;
    peer_manager_.UpdateState(conn->peer_id, PeerState::Connected);

    SPDLOG_DEBUG("Handshake complete with peer {}", conn->peer_id);
}

void TcpTransport::ProcessReadBuffer(Connection* conn) {
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
        auto frame_result = Codec::FrameSize(buf, config_.max_message_size);
        if (!frame_result) {
            SPDLOG_WARN("Invalid frame: {}", frame_result.error().ToString());
            CloseConnection(conn);
            return;
        }

        size_t frame_size = *frame_result;
        if (frame_size == 0 || buf.size() < frame_size) {
            break;  // Need more data
        }

        auto decode_result = Codec::Decode(buf, config_.max_message_size);
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
        peer_manager_.RecordActivity(conn->peer_id);

        if (on_message_) {
            on_message_(std::move(msg));
        }
    }
}

// Static callbacks for libuv
void TcpTransport::OnNewConnection(uv_stream_t* server, int status) {
    auto* transport = static_cast<TcpTransport*>(server->data);
    if (status < 0) {
        SPDLOG_WARN("Accept error: {}", uv_strerror(status));
        return;
    }

    auto* conn = new Connection();
    conn->is_outgoing = false;
    conn->transport = transport;

    uv_tcp_init(&transport->loop_, &conn->handle);
    transport->handle_to_conn_[&conn->handle] = conn;

    if (uv_accept(server, reinterpret_cast<uv_stream_t*>(&conn->handle)) == 0) {
        uv_read_start(reinterpret_cast<uv_stream_t*>(&conn->handle), OnAlloc, OnRead);
        SPDLOG_DEBUG("Accepted incoming connection");
    } else {
        transport->CloseConnection(conn);
    }
}

void TcpTransport::OnConnect(uv_connect_t* req, int status) {
    auto* conn = static_cast<Connection*>(req->data);
    auto* transport = static_cast<TcpTransport*>(conn->transport);
    delete req;

    if (status < 0) {
        SPDLOG_WARN("Connect to {} failed: {}", conn->peer_id, uv_strerror(status));
        transport->peer_manager_.RecordFailure(conn->peer_id);
        transport->handle_to_conn_.erase(&conn->handle);

        if (transport->on_error_) {
            transport->on_error_(conn->peer_id, uv_strerror(status));
        }

        transport->CloseConnection(conn);
        return;
    }

    SPDLOG_DEBUG("Connected to peer {}", conn->peer_id);

    // Start reading
    uv_read_start(reinterpret_cast<uv_stream_t*>(&conn->handle), OnAlloc, OnRead);

    // Send handshake
    transport->SendHandshake(conn);
}

void TcpTransport::OnAlloc(uv_handle_t* /*handle*/, size_t suggested_size, uv_buf_t* buf) {
    buf->base = new char[suggested_size];
    buf->len = suggested_size;
}

void TcpTransport::OnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    auto* conn = static_cast<Connection*>(stream->data);
    auto* transport = static_cast<TcpTransport*>(conn->transport);

    if (nread < 0) {
        if (nread != UV_EOF) {
            SPDLOG_DEBUG("Read error: {}", uv_strerror(nread));
        }

        if (conn->peer_id != 0) {
            transport->peer_manager_.RecordFailure(conn->peer_id);
            transport->connections_.erase(conn->peer_id);

            if (transport->on_error_) {
                transport->on_error_(
                    conn->peer_id, nread == UV_EOF ? "connection closed" : uv_strerror(nread)
                );
            }
        }

        transport->handle_to_conn_.erase(&conn->handle);
        transport->CloseConnection(conn);
        delete[] buf->base;
        return;
    }

    if (nread > 0) {
        conn->read_buf.insert(conn->read_buf.end(), buf->base, buf->base + nread);
        transport->ProcessReadBuffer(conn);
    }

    delete[] buf->base;
}

void TcpTransport::OnReconnectTimer(uv_timer_t* timer) {
    auto* transport = static_cast<TcpTransport*>(timer->data);
    if (transport->stopped_) {
        return;
    }

    for (auto peer_id : transport->peer_manager_.GetPeersToReconnect()) {
        transport->TryConnect(peer_id);
    }
}

}  // namespace raftpp::rpc
