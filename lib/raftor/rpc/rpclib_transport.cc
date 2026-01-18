#include "raftpp/raftor/rpc/rpclib_transport.h"

#include <spdlog/spdlog.h>

#include "raftpp/raftor/rpc/codec.h"

using raftpp::RaftError;
using raftpp::Result;

namespace raftpp::raftor::rpc {

RpclibTransport::RpclibTransport(TransportConfig config) : config_(std::move(config)) {}

RpclibTransport::~RpclibTransport() {
    RpclibTransport::Stop();
}

Result<void> RpclibTransport::Start() {
    if (running_) {
        return {};
    }

    // Parse listen address
    auto addr_result = ParseAddress(config_.listen_addr);
    if (!addr_result) {
        return std::unexpected(addr_result.error());
    }
    auto [host, port] = *addr_result;

    try {
        // Create server
        server_ = std::make_unique<::rpc::server>(host, port);

        // Bind the raft message handler
        server_->bind("raft_msg", [this](uint64_t from, const std::string& data) {
            OnRaftMessage(from, data);
        });

        // Start server in background with 1 worker thread
        server_->async_run(1);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Failed to start rpclib server: {}", e.what());
        return RaftError(RpcErrorCode::BindFailed);
    }

    running_ = true;
    stopped_ = false;

    SPDLOG_INFO("RpclibTransport started on {}", config_.listen_addr);

    // Connect to existing peers
    for (auto peer_id : peer_manager_.GetAllPeerIds()) {
        TryConnect(peer_id);
    }

    return {};
}

void RpclibTransport::Stop() {
    if (!running_ || stopped_) {
        return;
    }
    stopped_ = true;

    // Close all clients
    clients_.clear();

    // Stop server
    if (server_) {
        server_->stop();
        server_.reset();
    }

    running_ = false;
    SPDLOG_INFO("RpclibTransport stopped");
}

void RpclibTransport::AddPeer(uint64_t id, const std::string& addr) {
    peer_manager_.AddPeer(id, addr);
    if (running_) {
        TryConnect(id);
    }
}

void RpclibTransport::RemovePeer(uint64_t id) {
    peer_manager_.RemovePeer(id);
    clients_.erase(id);
}

void RpclibTransport::Send(std::span<const Message> messages) {
    for (const auto& msg : messages) {
        uint64_t to = msg.to();
        auto it = clients_.find(to);
        if (it == clients_.end()) {
            SPDLOG_DEBUG("Dropping message to {}: no client", to);
            continue;
        }

        auto* peer = peer_manager_.GetPeer(to);
        if (!peer || peer->state != PeerState::Connected) {
            SPDLOG_DEBUG("Dropping message to {}: peer not connected", to);
            continue;
        }

        // Check actual connection state
        auto conn_state = it->second->get_connection_state();
        if (conn_state != ::rpc::client::connection_state::connected) {
            SPDLOG_DEBUG("Dropping message to {}: rpc client not connected (state={})",
                        to, static_cast<int>(conn_state));
            continue;
        }

        try {
            // Serialize the protobuf message to binary string
            std::string data;
            msg.SerializeToString(&data);

            // Send as notification (fire-and-forget)
            it->second->send("raft_msg", config_.node_id, data);
            peer_manager_.RecordActivity(to);
        } catch (const std::exception& e) {
            SPDLOG_DEBUG("Send to {} failed: {}", to, e.what());
            peer_manager_.RecordFailure(to);
            clients_.erase(to);

            if (on_error_) {
                on_error_(to, e.what());
            }
        }
    }
}

void RpclibTransport::SetMessageCallback(MessageCallback cb) {
    on_message_ = std::move(cb);
}

void RpclibTransport::SetErrorCallback(ErrorCallback cb) {
    on_error_ = std::move(cb);
}

void RpclibTransport::Poll(std::chrono::milliseconds timeout) {
    // Process incoming messages from the queue
    {
        std::lock_guard lock(queue_mutex_);
        while (!incoming_queue_.empty()) {
            auto msg = std::move(incoming_queue_.front());
            incoming_queue_.pop();

            if (on_message_) {
                on_message_(std::move(msg));
            }
        }
    }

    // Check pending connections - update state if they've connected
    for (auto& [peer_id, client] : clients_) {
        auto* peer = peer_manager_.GetPeer(peer_id);
        if (peer && peer->state == PeerState::Disconnected) {
            auto state = client->get_connection_state();
            if (state == ::rpc::client::connection_state::connected) {
                peer_manager_.UpdateState(peer_id, PeerState::Connected);
                SPDLOG_DEBUG("Peer {} connection established", peer_id);
            }
        }
    }

    // Check for peers that need reconnection
    for (auto peer_id : peer_manager_.GetPeersToReconnect()) {
        TryConnect(peer_id);
    }

    // Sleep for the timeout duration to avoid busy-waiting
    if (timeout.count() > 0) {
        std::this_thread::sleep_for(timeout);
    }
}

void RpclibTransport::Run() {
    while (running_ && !stopped_) {
        Poll(std::chrono::milliseconds(100));
    }
}

void RpclibTransport::TryConnect(uint64_t peer_id) {
    auto* peer = peer_manager_.GetPeer(peer_id);
    if (!peer || peer->state != PeerState::Disconnected) {
        return;
    }

    // Already have a client for this peer - check if it's connected
    if (auto it = clients_.find(peer_id); it != clients_.end()) {
        auto state = it->second->get_connection_state();
        if (state == ::rpc::client::connection_state::connected) {
            peer_manager_.UpdateState(peer_id, PeerState::Connected);
            return;
        } else if (state == ::rpc::client::connection_state::initial) {
            // Still connecting, nothing to do
            return;
        } else {
            // Reset state - connection failed or disconnected
            clients_.erase(peer_id);
        }
    }

    auto addr_result = ParseAddress(peer->addr);
    if (!addr_result) {
        SPDLOG_WARN("Invalid peer address for {}: {}", peer_id, peer->addr);
        return;
    }
    auto [host, port] = *addr_result;

    peer_manager_.UpdateState(peer_id, PeerState::Connecting);

    try {
        auto client = std::make_unique<::rpc::client>(host, port);

        // Set timeout for connection
        client->set_timeout(static_cast<int64_t>(config_.connect_timeout.count()));

        // Check connection state
        auto state = client->get_connection_state();
        if (state == ::rpc::client::connection_state::connected) {
            clients_[peer_id] = std::move(client);
            peer_manager_.UpdateState(peer_id, PeerState::Connected);
            SPDLOG_DEBUG("Connected to peer {} at {}", peer_id, peer->addr);
        } else {
            // Connection is pending - store client and will check later
            SPDLOG_DEBUG("Connection to peer {} pending", peer_id);
            clients_[peer_id] = std::move(client);
            // Keep state as Connecting - we'll check again in Poll
            peer_manager_.UpdateState(peer_id, PeerState::Disconnected);
        }
    } catch (const std::exception& e) {
        SPDLOG_WARN("Connect to {} failed: {}", peer_id, e.what());
        peer_manager_.RecordFailure(peer_id);

        if (on_error_) {
            on_error_(peer_id, e.what());
        }
    }
}

void RpclibTransport::OnRaftMessage(uint64_t from, const std::string& data) {
    Message msg;
    if (!msg.ParseFromString(data)) {
        SPDLOG_WARN("Failed to parse message from {}", from);
        return;
    }

    peer_manager_.RecordActivity(from);

    // Queue the message for processing in Poll()
    std::lock_guard lock(queue_mutex_);
    incoming_queue_.push(std::move(msg));
}

}  // namespace raftpp::raftor::rpc
