#include "raftpp/raftor/rpc/rdma_transport.h"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <rdma/rdma_cma.h>
#include <spdlog/fmt/fmt.h>

#include "raftpp/core/capnp_util.h"
#include "raftpp/core/primitives.h"
#include "raftpp/core/types.h"
#include "raftpp/logging.h"
#include "raftpp/raftor/rpc/codec.h"
#include "raftpp/raftor/rpc/peer_manager.h"

namespace raftpp::raftor::rpc {
namespace {

constexpr size_t kMaxPendingIncomingMessages = 4096;
constexpr size_t kMaxPendingOutgoingBatches = 1024;
constexpr size_t kMaxPendingErrorEvents = 1024;
constexpr size_t kMaxPollBatch = 32;
constexpr int kListenBacklog = 128;
constexpr auto kCmPollInterval = std::chrono::milliseconds(10);

bool CopySockaddr(const sockaddr* addr, sockaddr_storage* storage) {
    if (!addr || !storage) {
        return false;
    }
    if (addr->sa_family == AF_INET) {
        std::memcpy(storage, addr, sizeof(sockaddr_in));
        return true;
    }
    if (addr->sa_family == AF_INET6) {
        std::memcpy(storage, addr, sizeof(sockaddr_in6));
        return true;
    }
    return false;
}

bool SockaddrAddressEqual(const sockaddr_storage& left, const sockaddr_storage& right) {
    if (left.ss_family != right.ss_family) {
        return false;
    }
    if (left.ss_family == AF_INET) {
        auto laddr = reinterpret_cast<const sockaddr_in*>(&left);
        auto raddr = reinterpret_cast<const sockaddr_in*>(&right);
        return std::memcmp(&laddr->sin_addr, &raddr->sin_addr, sizeof(in_addr)) == 0;
    }
    if (left.ss_family == AF_INET6) {
        auto laddr = reinterpret_cast<const sockaddr_in6*>(&left);
        auto raddr = reinterpret_cast<const sockaddr_in6*>(&right);
        return std::memcmp(&laddr->sin6_addr, &raddr->sin6_addr, sizeof(in6_addr)) == 0;
    }
    return false;
}

struct AddrInfoDeleter {
    void operator()(addrinfo* info) const {
        if (info) {
            freeaddrinfo(info);
        }
    }
};

}  // namespace

struct RdmaTransport::Impl {
    struct Connection;

    struct OutgoingBatch {
        uint64_t peer_id = 0;
        std::vector<Message> messages;
    };

    struct ErrorEvent {
        uint64_t peer_id = 0;
        std::string error;
    };

    struct RecvBuffer {
        std::unique_ptr<uint8_t[]> storage;
        size_t size = 0;
        ibv_mr* mr = nullptr;
        Connection* conn = nullptr;
    };

    struct SendBuffer {
        std::unique_ptr<uint8_t[]> storage;
        size_t size = 0;
        ibv_mr* mr = nullptr;
        Connection* conn = nullptr;
    };

    struct Connection {
        uint64_t peer_id = 0;
        std::string addr;
        rdma_cm_id* id = nullptr;
        ibv_pd* pd = nullptr;
        ibv_comp_channel* comp_channel = nullptr;
        ibv_cq* cq = nullptr;
        ibv_qp* qp = nullptr;
        bool is_active = false;
        bool established = false;
        bool handshake_done = false;
        size_t inflight_sends = 0;
        std::unordered_set<SendBuffer*> send_buffers;
        std::vector<std::unique_ptr<SendBuffer>> send_buffer_pool;
        std::vector<SendBuffer*> free_send_buffers;
        std::vector<std::unique_ptr<RecvBuffer>> recv_buffers;
    };

    explicit Impl(TransportConfig config, RdmaConfig rdma_config)
        : config_(std::move(config)), rdma_config_(std::move(rdma_config)) {}

    Result<void> Start();
    void Stop();
    void AddPeer(uint64_t id, const std::string& addr);
    void RemovePeer(uint64_t id);
    void Send(std::span<const Message> messages);
    void SetMessageCallback(MessageCallback cb);
    void SetErrorCallback(ErrorCallback cb);
    void Poll(std::chrono::milliseconds timeout);
    void Run();

  private:
    void RdmaLoop(std::promise<Result<void>> start_promise);
    Result<void> SetupListener();
    void TeardownListener();
    void PollCmEvents(std::chrono::milliseconds timeout);
    void HandleCmEvent(const rdma_cm_event& event);
    void HandleConnectRequest(rdma_cm_id* id, const rdma_conn_param& param);
    void HandleAddrResolved(rdma_cm_id* id);
    void HandleRouteResolved(rdma_cm_id* id);
    void HandleEstablished(rdma_cm_id* id);
    void HandleDisconnected(rdma_cm_id* id);
    void HandleConnectError(rdma_cm_id* id, const char* reason);

    bool SetupConnectionResources(Connection& conn);
    void ReleaseConnectionResources(Connection& conn);
    void CleanupConnection(Connection& conn);
    void RemoveConnection(Connection& conn);
    void DisconnectConnection(Connection& conn);

    void DrainOutgoing();
    void DrainRemovals();
    void DrainDisconnects();
    void PollCompletions();
    void PollCq(Connection& conn);

    void PostHandshake(Connection& conn);
    bool PostRecv(Connection& conn, RecvBuffer& buffer);
    bool PostSend(Connection& conn, std::span<const uint8_t> payload);
    void HandleRecv(RecvBuffer& buffer, size_t len);
    void ReleaseSendBuffer(Connection& conn, SendBuffer* buffer);
    bool HasIncomingCapacity();
    bool IsAllowedIncoming(rdma_cm_id* id);

    bool ShouldDial(uint64_t peer_id) const;
    bool ConnectPeer(uint64_t peer_id, const std::string& addr);
    void QueueDisconnect(Connection& conn);
    bool HasConnection(uint64_t peer_id) const;

    void EnqueueMessage(Message msg);
    void EnqueueError(uint64_t peer_id, std::string error);

    Result<sockaddr_storage> ResolveSockaddr(const std::string& addr) const;
    std::string GetPeerAddr(uint64_t peer_id) const;
    bool IsKnownPeer(uint64_t peer_id) const;
    void UpdatePeerState(uint64_t peer_id, PeerState state);
    void RecordPeerFailure(uint64_t peer_id);

    TransportConfig config_;
    RdmaConfig rdma_config_;

    mutable std::mutex peers_mutex_;
    PeerManager peer_manager_;

    std::mutex remove_mutex_;
    std::queue<uint64_t> remove_queue_;

    std::mutex outgoing_mutex_;
    std::queue<OutgoingBatch> outgoing_queue_;

    std::mutex incoming_mutex_;
    std::queue<Message> incoming_queue_;

    std::mutex error_mutex_;
    std::queue<ErrorEvent> error_queue_;

    std::mutex callback_mutex_;
    MessageCallback on_message_;
    ErrorCallback on_error_;

    std::mutex lifecycle_mutex_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopped_{false};
    std::thread rdma_thread_;

    rdma_event_channel* event_channel_ = nullptr;
    rdma_cm_id* listener_ = nullptr;

    std::unordered_map<rdma_cm_id*, std::unique_ptr<Connection>> connections_;
    std::unordered_map<uint64_t, Connection*> peer_connections_;
    std::unordered_map<uint64_t, rdma_cm_id*> connecting_peers_;
    std::vector<rdma_cm_id*> pending_disconnects_;
};

Result<void> RdmaTransport::Impl::Start() {
    if (config_.listen_addr.empty()) {
        return std::unexpected(RaftError(ConfigErrorCode::ListenAddressEmpty));
    }

    std::lock_guard lock(lifecycle_mutex_);
    if (running_) {
        return {};
    }

    running_ = true;
    stopped_ = false;

    std::promise<Result<void>> start_promise;
    auto start_future = start_promise.get_future();
    rdma_thread_ = std::thread([this, promise = std::move(start_promise)]() mutable {
        RdmaLoop(std::move(promise));
    });

    auto result = start_future.get();
    if (!result) {
        running_ = false;
        stopped_ = true;
        if (rdma_thread_.joinable()) {
            rdma_thread_.join();
        }
        return result;
    }

    RAFTPP_LOG_INFO("RdmaTransport started on {}", config_.listen_addr);
    return result;
}

void RdmaTransport::Impl::Stop() {
    std::lock_guard lock(lifecycle_mutex_);
    if (!running_ || stopped_) {
        return;
    }

    stopped_ = true;
    running_ = false;

    if (rdma_thread_.joinable() && rdma_thread_.get_id() != std::this_thread::get_id()) {
        rdma_thread_.join();
    }

    RAFTPP_LOG_INFO("RdmaTransport stopped");
}

void RdmaTransport::Impl::AddPeer(uint64_t id, const std::string& addr) {
    std::lock_guard lock(peers_mutex_);
    if (peer_manager_.HasPeer(id)) {
        peer_manager_.RemovePeer(id);
    }
    peer_manager_.AddPeer(id, addr);
}

void RdmaTransport::Impl::RemovePeer(uint64_t id) {
    {
        std::lock_guard lock(peers_mutex_);
        peer_manager_.RemovePeer(id);
    }
    std::lock_guard lock(remove_mutex_);
    remove_queue_.push(id);
}

void RdmaTransport::Impl::Send(std::span<const Message> messages) {
    Map<uint64_t, std::vector<Message>> batches;

    for (const auto& msg : messages) {
        const auto reader = capnp_util::reader<msg::Message>(msg);
        auto it = batches.find(reader.getTo());
        if (it == batches.end()) {
            it = batches.emplace(reader.getTo(), std::vector<Message>()).first;
        }
        it->second.push_back(CloneMessage(msg));
    }

    if (batches.empty()) {
        return;
    }

    std::vector<uint64_t> dropped_peers;
    {
        std::lock_guard lock(outgoing_mutex_);
        for (auto& [peer_id, batch] : batches) {
            if (batch.empty()) {
                continue;
            }
            if (outgoing_queue_.size() >= kMaxPendingOutgoingBatches) {
                dropped_peers.push_back(peer_id);
                continue;
            }
            outgoing_queue_.push(OutgoingBatch{peer_id, std::move(batch)});
        }
    }

    for (uint64_t peer_id : dropped_peers) {
        EnqueueError(
            peer_id,
            fmt::format(
                "outgoing_queue_ overflow (capacity={}), dropping batch", kMaxPendingOutgoingBatches
            )
        );
    }
}

void RdmaTransport::Impl::SetMessageCallback(MessageCallback cb) {
    std::lock_guard lock(callback_mutex_);
    on_message_ = std::move(cb);
}

void RdmaTransport::Impl::SetErrorCallback(ErrorCallback cb) {
    std::lock_guard lock(callback_mutex_);
    on_error_ = std::move(cb);
}

void RdmaTransport::Impl::Poll(std::chrono::milliseconds timeout) {
    std::queue<Message> incoming;
    {
        std::lock_guard lock(incoming_mutex_);
        std::swap(incoming, incoming_queue_);
    }
    MessageCallback message_cb;
    ErrorCallback error_cb;
    {
        std::lock_guard lock(callback_mutex_);
        message_cb = on_message_;
        error_cb = on_error_;
    }
    while (!incoming.empty()) {
        auto msg = std::move(incoming.front());
        incoming.pop();
        if (message_cb) {
            message_cb(std::move(msg));
        }
    }

    std::queue<ErrorEvent> errors;
    {
        std::lock_guard lock(error_mutex_);
        std::swap(errors, error_queue_);
    }
    while (!errors.empty()) {
        auto error = std::move(errors.front());
        errors.pop();
        if (error_cb) {
            error_cb(error.peer_id, std::move(error.error));
        }
    }

    if (timeout.count() > 0) {
        std::this_thread::sleep_for(timeout);
    }
}

void RdmaTransport::Impl::Run() {
    while (running_ && !stopped_) {
        Poll(std::chrono::milliseconds(100));
    }
}

void RdmaTransport::Impl::RdmaLoop(std::promise<Result<void>> start_promise) {
    bool started = false;
    auto set_start = [&](Result<void> result) {
        if (!started) {
            started = true;
            start_promise.set_value(std::move(result));
        }
    };

    auto setup = SetupListener();
    if (!setup) {
        set_start(setup);
        return;
    }
    set_start({});

    while (running_.load(std::memory_order_acquire) && !stopped_.load(std::memory_order_acquire)) {
        DrainRemovals();
        DrainDisconnects();
        PollCmEvents(kCmPollInterval);
        DrainOutgoing();
        PollCompletions();
        DrainDisconnects();

        std::vector<uint64_t> reconnect_ids;
        {
            std::lock_guard lock(peers_mutex_);
            reconnect_ids = peer_manager_.GetPeersToReconnect();
        }

        for (uint64_t peer_id : reconnect_ids) {
            if (!ShouldDial(peer_id)) {
                continue;
            }
            if (HasConnection(peer_id) || peer_connections_.contains(peer_id) ||
                connecting_peers_.contains(peer_id)) {
                continue;
            }
            auto addr = GetPeerAddr(peer_id);
            if (addr.empty()) {
                continue;
            }
            if (!ConnectPeer(peer_id, addr)) {
                RecordPeerFailure(peer_id);
            }
        }
    }

    for (auto& [_, conn] : connections_) {
        DisconnectConnection(*conn);
    }
    connections_.clear();
    peer_connections_.clear();
    connecting_peers_.clear();
    TeardownListener();
}

Result<void> RdmaTransport::Impl::SetupListener() {
    TeardownListener();

    event_channel_ = rdma_create_event_channel();
    if (!event_channel_) {
        RAFTPP_LOG_ERROR("rdma_create_event_channel failed: {}", strerror(errno));
        return std::unexpected(RaftError(RpcErrorCode::BindFailed));
    }

    int flags = fcntl(event_channel_->fd, F_GETFL, 0);
    if (flags >= 0) {
        if (fcntl(event_channel_->fd, F_SETFL, flags | O_NONBLOCK) != 0) {
            RAFTPP_LOG_WARN("Failed to set RDMA event channel non-blocking: {}", strerror(errno));
        }
    }

    if (rdma_create_id(event_channel_, &listener_, nullptr, RDMA_PS_TCP) != 0) {
        RAFTPP_LOG_ERROR("rdma_create_id failed: {}", strerror(errno));
        TeardownListener();
        return std::unexpected(RaftError(RpcErrorCode::BindFailed));
    }

    auto addr_result = ResolveSockaddr(config_.listen_addr);
    if (!addr_result) {
        TeardownListener();
        return std::unexpected(addr_result.error());
    }

    if (rdma_bind_addr(listener_, reinterpret_cast<sockaddr*>(&addr_result.value())) != 0) {
        RAFTPP_LOG_ERROR("rdma_bind_addr failed: {}", strerror(errno));
        TeardownListener();
        return std::unexpected(RaftError(RpcErrorCode::BindFailed));
    }

    if (rdma_listen(listener_, kListenBacklog) != 0) {
        RAFTPP_LOG_ERROR("rdma_listen failed: {}", strerror(errno));
        TeardownListener();
        return std::unexpected(RaftError(RpcErrorCode::ListenFailed));
    }

    return {};
}

void RdmaTransport::Impl::TeardownListener() {
    if (listener_) {
        rdma_destroy_id(listener_);
        listener_ = nullptr;
    }
    if (event_channel_) {
        rdma_destroy_event_channel(event_channel_);
        event_channel_ = nullptr;
    }
}

void RdmaTransport::Impl::PollCmEvents(std::chrono::milliseconds timeout) {
    if (!event_channel_) {
        return;
    }

    pollfd pfd{event_channel_->fd, POLLIN, 0};
    int ret = poll(&pfd, 1, static_cast<int>(timeout.count()));
    if (ret <= 0 || !(pfd.revents & POLLIN)) {
        return;
    }

    while (true) {
        rdma_cm_event* event = nullptr;
        if (rdma_get_cm_event(event_channel_, &event) != 0) {
            if (errno != EAGAIN) {
                RAFTPP_LOG_WARN("rdma_get_cm_event failed: {}", strerror(errno));
            }
            break;
        }

        HandleCmEvent(*event);
        rdma_ack_cm_event(event);
    }
}

void RdmaTransport::Impl::HandleCmEvent(const rdma_cm_event& event) {
    switch (event.event) {
        case RDMA_CM_EVENT_CONNECT_REQUEST:
            HandleConnectRequest(event.id, event.param.conn);
            break;
        case RDMA_CM_EVENT_ADDR_RESOLVED:
            HandleAddrResolved(event.id);
            break;
        case RDMA_CM_EVENT_ROUTE_RESOLVED:
            HandleRouteResolved(event.id);
            break;
        case RDMA_CM_EVENT_ESTABLISHED:
            HandleEstablished(event.id);
            break;
        case RDMA_CM_EVENT_DISCONNECTED:
            HandleDisconnected(event.id);
            break;
        case RDMA_CM_EVENT_REJECTED:
            HandleConnectError(event.id, "rejected");
            break;
        case RDMA_CM_EVENT_ADDR_ERROR:
            HandleConnectError(event.id, "addr_error");
            break;
        case RDMA_CM_EVENT_ROUTE_ERROR:
            HandleConnectError(event.id, "route_error");
            break;
        case RDMA_CM_EVENT_CONNECT_ERROR:
            HandleConnectError(event.id, "connect_error");
            break;
        case RDMA_CM_EVENT_UNREACHABLE:
            HandleConnectError(event.id, "unreachable");
            break;
        default:
            RAFTPP_LOG_DEBUG("Unhandled RDMA CM event {}", static_cast<int>(event.event));
            break;
    }
}

void RdmaTransport::Impl::HandleConnectRequest(rdma_cm_id* id, const rdma_conn_param& param) {
    if (!id) {
        return;
    }
    if (!HasIncomingCapacity()) {
        RAFTPP_LOG_WARN("rdma reject incoming request: capacity exceeded");
        if (rdma_reject(id, nullptr, 0) != 0) {
            RAFTPP_LOG_WARN("rdma_reject failed for incoming request: {}", strerror(errno));
        }
        rdma_destroy_id(id);
        return;
    }
    if (!IsAllowedIncoming(id)) {
        RAFTPP_LOG_WARN("rdma reject incoming request: unknown peer");
        if (rdma_reject(id, nullptr, 0) != 0) {
            RAFTPP_LOG_WARN("rdma_reject failed for incoming request: {}", strerror(errno));
        }
        rdma_destroy_id(id);
        return;
    }

    auto conn = std::make_unique<Connection>();
    conn->id = id;
    conn->is_active = false;
    conn->established = false;
    conn->handshake_done = false;
    id->context = conn.get();

    if (!SetupConnectionResources(*conn)) {
        rdma_reject(id, nullptr, 0);
        CleanupConnection(*conn);
        return;
    }

    rdma_conn_param reply{};
    reply.initiator_depth = param.initiator_depth;
    reply.responder_resources = param.responder_resources;
    reply.retry_count = param.retry_count;
    reply.rnr_retry_count = param.rnr_retry_count;

    if (rdma_accept(id, &reply) != 0) {
        RAFTPP_LOG_ERROR("rdma_accept failed: {}", strerror(errno));
        CleanupConnection(*conn);
        return;
    }

    connections_.emplace(id, std::move(conn));
}

bool RdmaTransport::Impl::HasIncomingCapacity() {
    std::lock_guard lock(peers_mutex_);
    size_t peer_count = peer_manager_.Size();
    if (peer_count == 0) {
        return false;
    }
    size_t max_connections = peer_count * 2;
    return connections_.size() < max_connections;
}

bool RdmaTransport::Impl::IsAllowedIncoming(rdma_cm_id* id) {
    const sockaddr* peer_addr = rdma_get_peer_addr(id);
    sockaddr_storage remote{};
    if (!CopySockaddr(peer_addr, &remote)) {
        return false;
    }

    std::vector<std::string> addrs;
    {
        std::lock_guard lock(peers_mutex_);
        auto ids = peer_manager_.GetAllPeerIds();
        addrs.reserve(ids.size());
        for (auto peer_id : ids) {
            const auto* peer = peer_manager_.GetPeer(peer_id);
            if (!peer || peer->addr.empty()) {
                continue;
            }
            addrs.push_back(peer->addr);
        }
    }
    if (addrs.empty()) {
        return false;
    }

    for (const auto& addr : addrs) {
        auto resolved = ResolveSockaddr(addr);
        if (!resolved) {
            continue;
        }
        if (SockaddrAddressEqual(remote, resolved.value())) {
            return true;
        }
    }
    return false;
}

void RdmaTransport::Impl::HandleAddrResolved(rdma_cm_id* id) {
    if (rdma_resolve_route(id, static_cast<int>(config_.connect_timeout.count())) != 0) {
        RAFTPP_LOG_WARN("rdma_resolve_route failed: {}", strerror(errno));
        HandleConnectError(id, "resolve_route");
    }
}

void RdmaTransport::Impl::HandleRouteResolved(rdma_cm_id* id) {
    auto* conn = static_cast<Connection*>(id->context);
    if (!conn) {
        HandleConnectError(id, "missing_context");
        return;
    }

    if (!SetupConnectionResources(*conn)) {
        HandleConnectError(id, "setup_resources");
        return;
    }

    rdma_conn_param param{};
    param.initiator_depth = 1;
    param.responder_resources = 1;
    param.retry_count = 3;
    param.rnr_retry_count = 3;

    if (rdma_connect(id, &param) != 0) {
        RAFTPP_LOG_WARN("rdma_connect failed: {}", strerror(errno));
        HandleConnectError(id, "connect");
    }
}

void RdmaTransport::Impl::HandleEstablished(rdma_cm_id* id) {
    auto* conn = static_cast<Connection*>(id->context);
    if (!conn) {
        return;
    }

    conn->established = true;
    if (conn->peer_id != 0) {
        connecting_peers_.erase(conn->peer_id);
    }
    PostHandshake(*conn);
}

void RdmaTransport::Impl::HandleDisconnected(rdma_cm_id* id) {
    auto* conn = static_cast<Connection*>(id->context);
    if (!conn) {
        rdma_destroy_id(id);
        return;
    }

    uint64_t peer_id = conn->peer_id;
    RemoveConnection(*conn);
    if (peer_id != 0 && ShouldDial(peer_id)) {
        RecordPeerFailure(peer_id);
    }
}

void RdmaTransport::Impl::HandleConnectError(rdma_cm_id* id, const char* reason) {
    if (!id) {
        RAFTPP_LOG_INFO("RDMA connect error (peer=0, reason={})", reason);
        return;
    }
    auto* conn = static_cast<Connection*>(id->context);
    uint64_t peer_id = conn ? conn->peer_id : 0;
    if (peer_id == 0) {
        RAFTPP_LOG_INFO("RDMA connect error (peer={}, reason={})", peer_id, reason);
    } else {
        RAFTPP_LOG_WARN("RDMA connect error (peer={}, reason={})", peer_id, reason);
    }

    if (conn) {
        RemoveConnection(*conn);
    } else {
        rdma_destroy_id(id);
    }

    if (peer_id != 0 && ShouldDial(peer_id)) {
        RecordPeerFailure(peer_id);
    }
}

bool RdmaTransport::Impl::SetupConnectionResources(Connection& conn) {
    if (!conn.id) {
        return false;
    }

    conn.pd = ibv_alloc_pd(conn.id->verbs);
    if (!conn.pd) {
        RAFTPP_LOG_ERROR("ibv_alloc_pd failed: {}", strerror(errno));
        return false;
    }

    conn.comp_channel = ibv_create_comp_channel(conn.id->verbs);
    if (!conn.comp_channel) {
        RAFTPP_LOG_ERROR("ibv_create_comp_channel failed: {}", strerror(errno));
        ReleaseConnectionResources(conn);
        return false;
    }
    int flags = fcntl(conn.comp_channel->fd, F_GETFL, 0);
    if (flags >= 0) {
        if (fcntl(conn.comp_channel->fd, F_SETFL, flags | O_NONBLOCK) != 0) {
            RAFTPP_LOG_WARN(
                "Failed to set RDMA completion channel non-blocking: {}", strerror(errno)
            );
        }
    }

    conn.cq = ibv_create_cq(
        conn.id->verbs, static_cast<int>(rdma_config_.cq_depth), &conn, conn.comp_channel, 0
    );
    if (!conn.cq) {
        RAFTPP_LOG_ERROR("ibv_create_cq failed: {}", strerror(errno));
        ReleaseConnectionResources(conn);
        return false;
    }
    if (ibv_req_notify_cq(conn.cq, 0) != 0) {
        RAFTPP_LOG_ERROR("ibv_req_notify_cq failed: {}", strerror(errno));
        ReleaseConnectionResources(conn);
        return false;
    }

    ibv_qp_init_attr qp_attr{};
    qp_attr.send_cq = conn.cq;
    qp_attr.recv_cq = conn.cq;
    qp_attr.cap.max_send_wr = rdma_config_.qp_depth;
    qp_attr.cap.max_recv_wr = rdma_config_.qp_depth;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;
    qp_attr.cap.max_inline_data = static_cast<uint32_t>(rdma_config_.max_inline_data);
    qp_attr.qp_type = IBV_QPT_RC;

    if (rdma_create_qp(conn.id, conn.pd, &qp_attr) != 0) {
        RAFTPP_LOG_ERROR("rdma_create_qp failed: {}", strerror(errno));
        ReleaseConnectionResources(conn);
        return false;
    }

    conn.qp = conn.id->qp;

    conn.recv_buffers.reserve(rdma_config_.recv_buffer_count);
    for (size_t i = 0; i < rdma_config_.recv_buffer_count; ++i) {
        auto buffer = std::make_unique<RecvBuffer>();
        buffer->size = rdma_config_.buffer_size;
        buffer->storage = std::make_unique<uint8_t[]>(buffer->size);
        buffer->mr =
            ibv_reg_mr(conn.pd, buffer->storage.get(), buffer->size, IBV_ACCESS_LOCAL_WRITE);
        if (!buffer->mr) {
            RAFTPP_LOG_ERROR("ibv_reg_mr failed: {}", strerror(errno));
            ReleaseConnectionResources(conn);
            return false;
        }
        buffer->conn = &conn;
        if (!PostRecv(conn, *buffer)) {
            ReleaseConnectionResources(conn);
            return false;
        }
        conn.recv_buffers.push_back(std::move(buffer));
    }

    conn.send_buffer_pool.reserve(rdma_config_.send_buffer_count);
    conn.free_send_buffers.reserve(rdma_config_.send_buffer_count);
    for (size_t i = 0; i < rdma_config_.send_buffer_count; ++i) {
        auto buffer = std::make_unique<SendBuffer>();
        buffer->size = rdma_config_.buffer_size;
        buffer->storage = std::make_unique<uint8_t[]>(buffer->size);
        buffer->mr =
            ibv_reg_mr(conn.pd, buffer->storage.get(), buffer->size, IBV_ACCESS_LOCAL_WRITE);
        if (!buffer->mr) {
            RAFTPP_LOG_ERROR("ibv_reg_mr failed: {}", strerror(errno));
            ReleaseConnectionResources(conn);
            return false;
        }
        buffer->conn = &conn;
        conn.free_send_buffers.push_back(buffer.get());
        conn.send_buffer_pool.push_back(std::move(buffer));
    }

    return true;
}

void RdmaTransport::Impl::ReleaseConnectionResources(Connection& conn) {
    if (conn.qp && conn.id) {
        rdma_destroy_qp(conn.id);
        conn.qp = nullptr;
    }

    conn.send_buffers.clear();
    conn.free_send_buffers.clear();
    for (auto& buffer : conn.send_buffer_pool) {
        if (buffer && buffer->mr) {
            ibv_dereg_mr(buffer->mr);
            buffer->mr = nullptr;
        }
    }
    conn.send_buffer_pool.clear();

    for (auto& buffer : conn.recv_buffers) {
        if (buffer && buffer->mr) {
            ibv_dereg_mr(buffer->mr);
            buffer->mr = nullptr;
        }
    }
    conn.recv_buffers.clear();

    if (conn.cq) {
        ibv_destroy_cq(conn.cq);
        conn.cq = nullptr;
    }

    if (conn.comp_channel) {
        ibv_destroy_comp_channel(conn.comp_channel);
        conn.comp_channel = nullptr;
    }

    if (conn.pd) {
        ibv_dealloc_pd(conn.pd);
        conn.pd = nullptr;
    }
}

void RdmaTransport::Impl::CleanupConnection(Connection& conn) {
    ReleaseConnectionResources(conn);

    if (conn.id) {
        rdma_destroy_id(conn.id);
        conn.id = nullptr;
    }
}

void RdmaTransport::Impl::DisconnectConnection(Connection& conn) {
    if (conn.id) {
        rdma_disconnect(conn.id);
    }
    CleanupConnection(conn);
}

void RdmaTransport::Impl::RemoveConnection(Connection& conn) {
    auto peer_id = conn.peer_id;
    if (peer_id != 0) {
        bool removed = false;
        auto it = peer_connections_.find(peer_id);
        if (it != peer_connections_.end() && it->second == &conn) {
            peer_connections_.erase(it);
            removed = true;
        }
        auto connecting_it = connecting_peers_.find(peer_id);
        if (connecting_it != connecting_peers_.end() && connecting_it->second == conn.id) {
            connecting_peers_.erase(connecting_it);
        }
        if (removed) {
            UpdatePeerState(peer_id, PeerState::Disconnected);
        }
    }

    auto it = connections_.find(conn.id);
    if (it != connections_.end()) {
        DisconnectConnection(*it->second);
        connections_.erase(it);
    }
}

void RdmaTransport::Impl::DrainOutgoing() {
    std::queue<OutgoingBatch> outgoing;
    {
        std::lock_guard lock(outgoing_mutex_);
        std::swap(outgoing, outgoing_queue_);
    }

    while (!outgoing.empty()) {
        auto batch = std::move(outgoing.front());
        outgoing.pop();

        auto it = peer_connections_.find(batch.peer_id);
        if (it == peer_connections_.end()) {
            EnqueueError(batch.peer_id, "peer not connected");
            continue;
        }

        Connection* conn = it->second;
        if (!conn || !conn->handshake_done) {
            EnqueueError(batch.peer_id, "peer handshake not complete");
            continue;
        }

        for (const auto& msg : batch.messages) {
            if (conn->inflight_sends >= rdma_config_.send_buffer_count) {
                EnqueueError(batch.peer_id, "rdma send backpressure");
                break;
            }

            auto encoded = Codec::Encode(msg, config_.node_id, batch.peer_id, 0);
            if (encoded.size() > rdma_config_.buffer_size) {
                EnqueueError(batch.peer_id, "message exceeds rdma buffer size");
                continue;
            }

            if (!PostSend(*conn, encoded)) {
                EnqueueError(batch.peer_id, "rdma send failed");
            }
        }
    }
}

void RdmaTransport::Impl::DrainRemovals() {
    std::queue<uint64_t> removals;
    {
        std::lock_guard lock(remove_mutex_);
        std::swap(removals, remove_queue_);
    }

    while (!removals.empty()) {
        uint64_t peer_id = removals.front();
        removals.pop();
        auto it = peer_connections_.find(peer_id);
        if (it != peer_connections_.end()) {
            RemoveConnection(*it->second);
        }
    }
}

void RdmaTransport::Impl::DrainDisconnects() {
    if (pending_disconnects_.empty()) {
        return;
    }
    auto pending = std::move(pending_disconnects_);
    pending_disconnects_.clear();

    for (auto* id : pending) {
        auto it = connections_.find(id);
        if (it == connections_.end()) {
            continue;
        }
        RemoveConnection(*it->second);
    }
}

void RdmaTransport::Impl::PollCompletions() {
    if (connections_.empty()) {
        return;
    }

    std::vector<pollfd> pollfds;
    std::vector<ibv_comp_channel*> channels;
    pollfds.reserve(connections_.size());
    channels.reserve(connections_.size());

    for (auto& [_, conn_ptr] : connections_) {
        if (!conn_ptr || !conn_ptr->comp_channel) {
            continue;
        }
        pollfds.push_back(pollfd{conn_ptr->comp_channel->fd, POLLIN, 0});
        channels.push_back(conn_ptr->comp_channel);
    }

    if (pollfds.empty()) {
        return;
    }

    int ready = poll(pollfds.data(), static_cast<nfds_t>(pollfds.size()), 0);
    if (ready <= 0) {
        return;
    }

    for (size_t i = 0; i < pollfds.size(); ++i) {
        if ((pollfds[i].revents & POLLIN) == 0) {
            continue;
        }
        ibv_cq* cq = nullptr;
        void* context = nullptr;
        while (ibv_get_cq_event(channels[i], &cq, &context) == 0) {
            ibv_ack_cq_events(cq, 1);
            if (ibv_req_notify_cq(cq, 0) != 0) {
                RAFTPP_LOG_WARN("ibv_req_notify_cq failed: {}", strerror(errno));
            }
            auto* conn = static_cast<Connection*>(context);
            if (!conn || conn->cq != cq) {
                continue;
            }
            PollCq(*conn);
        }
        int err = errno;
        if (err != EAGAIN && err != 0) {
            RAFTPP_LOG_WARN("ibv_get_cq_event failed: {}", strerror(err));
        }
    }
}

void RdmaTransport::Impl::PollCq(Connection& conn) {
    if (!conn.cq) {
        return;
    }
    std::array<ibv_wc, kMaxPollBatch> wc{};
    while (true) {
        int count = ibv_poll_cq(conn.cq, static_cast<int>(wc.size()), wc.data());
        if (count <= 0) {
            break;
        }

        for (int i = 0; i < count; ++i) {
            const auto& completion = wc[static_cast<size_t>(i)];
            if (completion.status != IBV_WC_SUCCESS) {
                RAFTPP_LOG_WARN(
                    "RDMA completion error (peer={}, status={})", conn.peer_id,
                    static_cast<int>(completion.status)
                );
                if (completion.opcode == IBV_WC_SEND) {
                    auto* buffer = reinterpret_cast<SendBuffer*>(completion.wr_id);
                    ReleaseSendBuffer(conn, buffer);
                }
                QueueDisconnect(conn);
                continue;
            }

            if (completion.opcode == IBV_WC_RECV) {
                auto* buffer = reinterpret_cast<RecvBuffer*>(completion.wr_id);
                if (!buffer) {
                    continue;
                }
                HandleRecv(*buffer, completion.byte_len);
                PostRecv(conn, *buffer);
            } else if (completion.opcode == IBV_WC_SEND) {
                auto* buffer = reinterpret_cast<SendBuffer*>(completion.wr_id);
                if (!buffer) {
                    continue;
                }
                ReleaseSendBuffer(conn, buffer);
            }
        }
    }
}

void RdmaTransport::Impl::PostHandshake(Connection& conn) {
    auto hs = capnp_util::make<capnp::RpcHandshake>();
    auto hs_builder = capnp_util::builder<capnp::RpcHandshake>(hs);
    hs_builder.setVersion(HandshakeCodec::kVersion);
    hs_builder.setNodeId(config_.node_id);
    hs_builder.setClusterId(0);
    auto bytes = HandshakeCodec::Encode(hs);
    if (!PostSend(conn, bytes)) {
        EnqueueError(conn.peer_id, "rdma handshake send failed");
    }
}

bool RdmaTransport::Impl::PostRecv(Connection& conn, RecvBuffer& buffer) {
    ibv_sge sge{};
    sge.addr = reinterpret_cast<uint64_t>(buffer.storage.get());
    sge.length = static_cast<uint32_t>(buffer.size);
    sge.lkey = buffer.mr->lkey;

    ibv_recv_wr wr{};
    wr.wr_id = reinterpret_cast<uint64_t>(&buffer);
    wr.sg_list = &sge;
    wr.num_sge = 1;

    ibv_recv_wr* bad = nullptr;
    if (ibv_post_recv(conn.qp, &wr, &bad) != 0) {
        RAFTPP_LOG_WARN("ibv_post_recv failed: {}", strerror(errno));
        return false;
    }

    return true;
}

bool RdmaTransport::Impl::PostSend(Connection& conn, std::span<const uint8_t> payload) {
    if (payload.size() > rdma_config_.buffer_size) {
        RAFTPP_LOG_WARN("rdma send payload too large: {}", payload.size());
        return false;
    }
    if (conn.free_send_buffers.empty()) {
        RAFTPP_LOG_WARN("rdma send buffer pool exhausted");
        return false;
    }
    auto* buffer = conn.free_send_buffers.back();
    conn.free_send_buffers.pop_back();
    std::memcpy(buffer->storage.get(), payload.data(), payload.size());

    ibv_sge sge{};
    sge.addr = reinterpret_cast<uint64_t>(buffer->storage.get());
    sge.length = static_cast<uint32_t>(payload.size());
    sge.lkey = buffer->mr->lkey;

    ibv_send_wr wr{};
    wr.wr_id = reinterpret_cast<uint64_t>(buffer);
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;
    if (rdma_config_.max_inline_data > 0 && payload.size() <= rdma_config_.max_inline_data) {
        wr.send_flags |= IBV_SEND_INLINE;
    }

    ibv_send_wr* bad = nullptr;
    if (ibv_post_send(conn.qp, &wr, &bad) != 0) {
        RAFTPP_LOG_WARN("ibv_post_send failed: {}", strerror(errno));
        conn.free_send_buffers.push_back(buffer);
        return false;
    }

    ++conn.inflight_sends;
    conn.send_buffers.insert(buffer);
    return true;
}

void RdmaTransport::Impl::ReleaseSendBuffer(Connection& conn, SendBuffer* buffer) {
    if (!buffer) {
        return;
    }
    if (conn.inflight_sends > 0) {
        --conn.inflight_sends;
    }
    conn.send_buffers.erase(buffer);
    conn.free_send_buffers.push_back(buffer);
}

void RdmaTransport::Impl::HandleRecv(RecvBuffer& buffer, size_t len) {
    if (len < sizeof(uint32_t)) {
        EnqueueError(buffer.conn ? buffer.conn->peer_id : 0, "rdma recv too short");
        return;
    }

    uint32_t magic = 0;
    std::memcpy(&magic, buffer.storage.get(), sizeof(magic));
    std::span<const uint8_t> payload(buffer.storage.get(), len);

    if (magic == HandshakeCodec::kMagic) {
        auto result = HandshakeCodec::Decode(payload);
        if (!result) {
            EnqueueError(buffer.conn ? buffer.conn->peer_id : 0, "handshake decode failed");
            if (buffer.conn) {
                QueueDisconnect(*buffer.conn);
            }
            return;
        }

        auto& [hs, consumed] = *result;
        if (consumed != len) {
            EnqueueError(buffer.conn ? buffer.conn->peer_id : 0, "handshake size mismatch");
            if (buffer.conn) {
                QueueDisconnect(*buffer.conn);
            }
            return;
        }

        auto reader = capnp_util::reader<capnp::RpcHandshake>(hs);
        if (reader.getVersion() != HandshakeCodec::kVersion) {
            EnqueueError(buffer.conn ? buffer.conn->peer_id : 0, "handshake version mismatch");
            if (buffer.conn) {
                QueueDisconnect(*buffer.conn);
            }
            return;
        }

        uint64_t peer_id = reader.getNodeId();
        if (peer_id == 0 || peer_id == config_.node_id) {
            EnqueueError(peer_id, "invalid handshake node id");
            if (buffer.conn) {
                QueueDisconnect(*buffer.conn);
            }
            return;
        }

        auto* conn = buffer.conn;
        if (!conn) {
            return;
        }

        if (conn->peer_id != 0 && conn->peer_id != peer_id) {
            EnqueueError(peer_id, "handshake peer id mismatch");
            QueueDisconnect(*conn);
            return;
        }

        if (!IsKnownPeer(peer_id)) {
            EnqueueError(peer_id, "unknown peer id");
            QueueDisconnect(*conn);
            return;
        }

        auto it = peer_connections_.find(peer_id);
        if (it != peer_connections_.end() && it->second != conn) {
            bool prefer_active = ShouldDial(peer_id);
            Connection* existing = it->second;
            if (prefer_active) {
                if (conn->is_active) {
                    QueueDisconnect(*existing);
                } else {
                    QueueDisconnect(*conn);
                    return;
                }
            } else {
                if (!conn->is_active) {
                    QueueDisconnect(*existing);
                } else {
                    QueueDisconnect(*conn);
                    return;
                }
            }
        }

        conn->peer_id = peer_id;
        conn->handshake_done = true;
        peer_connections_[peer_id] = conn;
        UpdatePeerState(peer_id, PeerState::Connected);
        return;
    }

    if (magic == Codec::kMagic) {
        auto* conn = buffer.conn;
        if (!conn || !conn->handshake_done) {
            EnqueueError(conn ? conn->peer_id : 0, "message received before handshake");
            if (conn) {
                QueueDisconnect(*conn);
            }
            return;
        }
        auto result = Codec::Decode(payload, config_.max_message_size);
        if (!result) {
            EnqueueError(conn ? conn->peer_id : 0, "message decode failed");
            return;
        }

        if (result->bytes_consumed != len) {
            EnqueueError(conn ? conn->peer_id : 0, "message size mismatch");
            return;
        }

        auto msg_reader = capnp_util::reader<msg::Message>(result->message);
        if (msg_reader.getFrom() != conn->peer_id) {
            EnqueueError(conn->peer_id, "message from mismatch");
            QueueDisconnect(*conn);
            return;
        }
        EnqueueMessage(std::move(result->message));
        return;
    }

    EnqueueError(buffer.conn ? buffer.conn->peer_id : 0, "unknown rdma frame magic");
}

bool RdmaTransport::Impl::ShouldDial(uint64_t peer_id) const {
    return config_.node_id < peer_id;
}

bool RdmaTransport::Impl::HasConnection(uint64_t peer_id) const {
    for (const auto& [_, conn] : connections_) {
        if (conn && conn->peer_id == peer_id) {
            return true;
        }
    }
    return false;
}

bool RdmaTransport::Impl::ConnectPeer(uint64_t peer_id, const std::string& addr) {
    if (!event_channel_) {
        return false;
    }

    auto conn = std::make_unique<Connection>();
    conn->peer_id = peer_id;
    conn->addr = addr;
    conn->is_active = true;

    rdma_cm_id* id = nullptr;
    if (rdma_create_id(event_channel_, &id, conn.get(), RDMA_PS_TCP) != 0) {
        RAFTPP_LOG_WARN("rdma_create_id failed: {}", strerror(errno));
        return false;
    }
    conn->id = id;
    id->context = conn.get();

    auto addr_result = ResolveSockaddr(addr);
    if (!addr_result) {
        rdma_destroy_id(id);
        return false;
    }

    if (rdma_resolve_addr(
            id, nullptr, reinterpret_cast<sockaddr*>(&addr_result.value()),
            static_cast<int>(config_.connect_timeout.count())
        ) != 0) {
        RAFTPP_LOG_WARN("rdma_resolve_addr failed: {}", strerror(errno));
        rdma_destroy_id(id);
        return false;
    }

    connections_.emplace(id, std::move(conn));
    connecting_peers_[peer_id] = id;
    UpdatePeerState(peer_id, PeerState::Connecting);
    return true;
}

void RdmaTransport::Impl::QueueDisconnect(Connection& conn) {
    if (!conn.id) {
        return;
    }
    pending_disconnects_.push_back(conn.id);
}

void RdmaTransport::Impl::EnqueueMessage(Message msg) {
    uint64_t peer_id = 0;
    {
        std::lock_guard lock(incoming_mutex_);
        if (incoming_queue_.size() >= kMaxPendingIncomingMessages) {
            peer_id = capnp_util::reader<msg::Message>(msg).getFrom();
        } else {
            incoming_queue_.push(std::move(msg));
            return;
        }
    }
    EnqueueError(
        peer_id,
        fmt::format(
            "incoming_queue_ overflow (capacity={}), dropping message", kMaxPendingIncomingMessages
        )
    );
}

void RdmaTransport::Impl::EnqueueError(uint64_t peer_id, std::string error) {
    std::lock_guard lock(error_mutex_);
    if (error_queue_.size() >= kMaxPendingErrorEvents) {
        RAFTPP_LOG_WARN(
            "error_queue_ overflow (capacity={}) for peer {}, dropping error: {}",
            kMaxPendingErrorEvents, peer_id, error
        );
        return;
    }
    error_queue_.push(ErrorEvent{peer_id, std::move(error)});
}

Result<sockaddr_storage> RdmaTransport::Impl::ResolveSockaddr(const std::string& addr) const {
    auto addr_result = ParseAddress(addr);
    if (!addr_result) {
        return std::unexpected(addr_result.error());
    }

    auto& [host, port] = *addr_result;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* info = nullptr;
    int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &info);
    if (rc != 0 || !info) {
        return std::unexpected(RaftError(RpcErrorCode::AddressPortInvalid));
    }

    std::unique_ptr<addrinfo, AddrInfoDeleter> guard(info);
    sockaddr_storage storage{};
    std::memcpy(&storage, info->ai_addr, info->ai_addrlen);
    return storage;
}

std::string RdmaTransport::Impl::GetPeerAddr(uint64_t peer_id) const {
    std::lock_guard lock(peers_mutex_);
    auto* peer = peer_manager_.GetPeer(peer_id);
    if (!peer) {
        return {};
    }
    return peer->addr;
}

bool RdmaTransport::Impl::IsKnownPeer(uint64_t peer_id) const {
    std::lock_guard lock(peers_mutex_);
    return peer_manager_.HasPeer(peer_id);
}

void RdmaTransport::Impl::UpdatePeerState(uint64_t peer_id, PeerState state) {
    std::lock_guard lock(peers_mutex_);
    peer_manager_.UpdateState(peer_id, state);
}

void RdmaTransport::Impl::RecordPeerFailure(uint64_t peer_id) {
    std::lock_guard lock(peers_mutex_);
    peer_manager_.RecordFailure(peer_id);
}

RdmaTransport::RdmaTransport(TransportConfig config, RdmaConfig rdma_config)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(rdma_config))) {}

RdmaTransport::~RdmaTransport() {
    RdmaTransport::Stop();
}

Result<void> RdmaTransport::Start() {
    return impl_->Start();
}

void RdmaTransport::Stop() {
    impl_->Stop();
}

void RdmaTransport::AddPeer(uint64_t id, const std::string& addr) {
    impl_->AddPeer(id, addr);
}

void RdmaTransport::RemovePeer(uint64_t id) {
    impl_->RemovePeer(id);
}

void RdmaTransport::Send(std::span<const Message> messages) {
    impl_->Send(messages);
}

void RdmaTransport::SetMessageCallback(MessageCallback cb) {
    impl_->SetMessageCallback(std::move(cb));
}

void RdmaTransport::SetErrorCallback(ErrorCallback cb) {
    impl_->SetErrorCallback(std::move(cb));
}

void RdmaTransport::Poll(std::chrono::milliseconds timeout) {
    impl_->Poll(timeout);
}

void RdmaTransport::Run() {
    impl_->Run();
}

}  // namespace raftpp::raftor::rpc
