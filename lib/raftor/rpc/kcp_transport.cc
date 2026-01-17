#include "raftor/rpc/kcp_transport.h"

#include <cstring>

#include <spdlog/spdlog.h>

namespace raftpp::rpc {

using raftpp::RaftError;
using raftpp::Result;
using raftpp::RpcErrorCode;

namespace {

/// Magic number for KCP handshake packets
constexpr uint32_t kKcpHandshakeMagic = 0x4B435048;  // "KCPH"

/// Handshake packet types
enum class KcpHandshakeType : uint8_t {
    Request = 0,
    Response = 1,
    Reject = 2,
};

/// KCP handshake packet structure (17 bytes + padding to 20)
struct KcpHandshakePacket {
    uint32_t magic;
    uint8_t type;
    uint32_t conv;
    uint64_t node_id;
    uint8_t padding[3];

    static constexpr size_t kSize = 20;

    [[nodiscard]] std::vector<uint8_t> Encode() const {
        std::vector<uint8_t> buf(kSize);
        std::memcpy(buf.data(), &magic, 4);
        buf[4] = type;
        std::memcpy(buf.data() + 5, &conv, 4);
        std::memcpy(buf.data() + 9, &node_id, 8);
        return buf;
    }

    [[nodiscard]] static Result<KcpHandshakePacket> Decode(std::span<const uint8_t> buf) {
        if (buf.size() < kSize) {
            return RaftError(RpcErrorCode::HandshakeTooShort);
        }

        KcpHandshakePacket pkt;
        std::memcpy(&pkt.magic, buf.data(), 4);
        pkt.type = buf[4];
        std::memcpy(&pkt.conv, buf.data() + 5, 4);
        std::memcpy(&pkt.node_id, buf.data() + 9, 8);

        if (pkt.magic != kKcpHandshakeMagic) {
            return RaftError(RpcErrorCode::HandshakeInvalidMagic);
        }

        return pkt;
    }
};

/// Get current time in milliseconds
[[nodiscard]] uint32_t GetCurrentMs() {
    using namespace std::chrono;
    auto now = steady_clock::now();
    return static_cast<uint32_t>(duration_cast<milliseconds>(now.time_since_epoch()).count());
}

/// Compare two sockaddr structures
[[nodiscard]] bool SockaddrEqual(const sockaddr_storage& a, const sockaddr_storage& b) {
    if (a.ss_family != b.ss_family) {
        return false;
    }

    if (a.ss_family == AF_INET) {
        const auto* a4 = reinterpret_cast<const sockaddr_in*>(&a);
        const auto* b4 = reinterpret_cast<const sockaddr_in*>(&b);
        return a4->sin_port == b4->sin_port && a4->sin_addr.s_addr == b4->sin_addr.s_addr;
    }

    if (a.ss_family == AF_INET6) {
        const auto* a6 = reinterpret_cast<const sockaddr_in6*>(&a);
        const auto* b6 = reinterpret_cast<const sockaddr_in6*>(&b);
        return a6->sin6_port == b6->sin6_port &&
            std::memcmp(&a6->sin6_addr, &b6->sin6_addr, 16) == 0;
    }

    return false;
}

/// Hash a sockaddr_storage
[[nodiscard]] size_t HashSockaddr(const sockaddr_storage& addr) {
    size_t h = std::hash<int>{}(addr.ss_family);

    if (addr.ss_family == AF_INET) {
        const auto* a4 = reinterpret_cast<const sockaddr_in*>(&addr);
        h ^= std::hash<uint32_t>{}(a4->sin_addr.s_addr) << 1;
        h ^= std::hash<uint16_t>{}(a4->sin_port) << 2;
    } else if (addr.ss_family == AF_INET6) {
        const auto* a6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        for (int i = 0; i < 16; i += 4) {
            uint32_t chunk;
            std::memcpy(&chunk, &a6->sin6_addr.s6_addr[i], 4);
            h ^= std::hash<uint32_t>{}(chunk) << (i / 4 + 1);
        }
        h ^= std::hash<uint16_t>{}(a6->sin6_port) << 5;
    }

    return h;
}

}  // namespace

// KcpSession destructor
KcpSession::~KcpSession() {
    if (kcp) {
        ikcp_release(kcp);
        kcp = nullptr;
    }
}

// AddrKey comparison
bool AddrKey::operator==(const AddrKey& other) const {
    return SockaddrEqual(addr, other.addr);
}

// AddrKeyHash
size_t AddrKeyHash::operator()(const AddrKey& k) const {
    return HashSockaddr(k.addr);
}

// KcpTransport implementation
KcpTransport::KcpTransport(TransportConfig config, KcpConfig kcp_config)
    : config_(std::move(config)), kcp_config_(kcp_config) {
    uv_loop_init(&loop_);
    udp_handle_.data = this;
    update_timer_.data = this;
    reconnect_timer_.data = this;
}

KcpTransport::~KcpTransport() {
    Stop();
    uv_loop_close(&loop_);
}

Result<void> KcpTransport::Start() {
    if (running_) {
        return {};
    }

    auto addr_result = ParseAddress(config_.listen_addr);
    if (!addr_result) {
        return std::unexpected(addr_result.error());
    }
    auto [host, port] = *addr_result;

    uv_udp_init(&loop_, &udp_handle_);

    sockaddr_in addr{};
    uv_ip4_addr(host.c_str(), port, &addr);

    int r = uv_udp_bind(&udp_handle_, reinterpret_cast<const sockaddr*>(&addr), 0);
    if (r != 0) {
        return RaftError(RpcErrorCode::UdpBindFailed);
    }

    r = uv_udp_recv_start(&udp_handle_, OnAlloc, OnRecv);
    if (r != 0) {
        return RaftError(RpcErrorCode::UdpRecvStartFailed);
    }

    uv_timer_init(&loop_, &update_timer_);
    uv_timer_start(&update_timer_, OnKcpUpdate, kcp_config_.interval, kcp_config_.interval);

    uv_timer_init(&loop_, &reconnect_timer_);
    uv_timer_start(&reconnect_timer_, OnReconnectTimer, 1000, 1000);

    running_ = true;
    stopped_ = false;

    SPDLOG_INFO("KCP Transport started on {}", config_.listen_addr);

    for (auto peer_id : peer_manager_.GetAllPeerIds()) {
        InitiateHandshake(peer_id);
    }

    return {};
}

void KcpTransport::Stop() {
    if (!running_ || stopped_) {
        return;
    }
    stopped_ = true;

    // Use sessions_by_conv_ to delete all sessions, as it contains all sessions
    // (sessions_by_peer_ may not contain sessions with peer_id == 0)
    for (auto& [_, session] : sessions_by_conv_) {
        delete session;
    }
    sessions_by_peer_.clear();
    sessions_by_conv_.clear();
    sessions_by_addr_.clear();

    uv_timer_stop(&update_timer_);
    uv_close(reinterpret_cast<uv_handle_t*>(&update_timer_), nullptr);

    uv_timer_stop(&reconnect_timer_);
    uv_close(reinterpret_cast<uv_handle_t*>(&reconnect_timer_), nullptr);

    uv_udp_recv_stop(&udp_handle_);
    uv_close(reinterpret_cast<uv_handle_t*>(&udp_handle_), nullptr);

    while (uv_loop_alive(&loop_)) {
        uv_run(&loop_, UV_RUN_ONCE);
    }

    running_ = false;
    SPDLOG_INFO("KCP Transport stopped");
}

void KcpTransport::AddPeer(uint64_t id, const std::string& addr_str) {
    peer_manager_.AddPeer(id, addr_str);

    auto addr_result = ParseAddress(addr_str);
    if (!addr_result) {
        SPDLOG_WARN("Invalid peer address for {}: {}", id, addr_str);
        return;
    }
    auto [host, port] = *addr_result;

    sockaddr_in addr{};
    uv_ip4_addr(host.c_str(), port, &addr);

    sockaddr_storage storage{};
    std::memcpy(&storage, &addr, sizeof(addr));
    peer_addresses_[id] = storage;

    if (running_) {
        InitiateHandshake(id);
    }
}

void KcpTransport::RemovePeer(uint64_t id) {
    peer_manager_.RemovePeer(id);
    peer_addresses_.erase(id);

    if (auto it = sessions_by_peer_.find(id); it != sessions_by_peer_.end()) {
        DestroySession(it->second);
    }
}

void KcpTransport::Send(std::span<const Message> messages) {
    for (const auto& msg : messages) {
        uint64_t to = msg.to();
        auto it = sessions_by_peer_.find(to);
        if (it == sessions_by_peer_.end() || !it->second->handshake_done) {
            SPDLOG_DEBUG("Dropping message to {}: not connected", to);
            continue;
        }

        auto buf = Codec::Encode(msg);
        int ret = ikcp_send(
            it->second->kcp, reinterpret_cast<const char*>(buf.data()), static_cast<int>(buf.size())
        );
        if (ret < 0) {
            SPDLOG_WARN("KCP send failed to {}: {}", to, ret);
        }
    }
}

void KcpTransport::SetMessageCallback(MessageCallback cb) {
    on_message_ = std::move(cb);
}

void KcpTransport::SetErrorCallback(ErrorCallback cb) {
    on_error_ = std::move(cb);
}

void KcpTransport::Poll(std::chrono::milliseconds timeout) {
    uv_run(&loop_, UV_RUN_NOWAIT);

    if (timeout.count() > 0) {
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
        uv_run(&loop_, UV_RUN_NOWAIT);
    }
}

void KcpTransport::Run() {
    uv_run(&loop_, UV_RUN_DEFAULT);
}

uint32_t KcpTransport::AllocateConvId() {
    while (sessions_by_conv_.contains(next_conv_id_)) {
        next_conv_id_++;
        if (next_conv_id_ == 0) {
            next_conv_id_ = 1;
        }
    }
    return next_conv_id_++;
}

KcpSession* KcpTransport::CreateSession(
    uint32_t conv, uint64_t peer_id, const sockaddr_storage& addr, socklen_t addr_len,
    bool is_initiator
) {
    auto* session = new KcpSession();
    session->conv = conv;
    session->peer_id = peer_id;
    session->remote_addr = addr;
    session->addr_len = addr_len;
    session->is_initiator = is_initiator;
    session->last_activity = std::chrono::steady_clock::now();
    session->transport = this;

    session->kcp = ikcp_create(conv, session);
    ikcp_setoutput(session->kcp, KcpOutput);
    ikcp_nodelay(
        session->kcp, kcp_config_.nodelay, kcp_config_.interval, kcp_config_.resend, kcp_config_.nc
    );
    ikcp_wndsize(session->kcp, kcp_config_.snd_wnd, kcp_config_.rcv_wnd);
    ikcp_setmtu(session->kcp, kcp_config_.mtu);

    sessions_by_conv_[conv] = session;
    if (peer_id != 0) {
        sessions_by_peer_[peer_id] = session;
    }
    AddrKey key{addr};
    sessions_by_addr_[key] = session;

    SPDLOG_DEBUG("Created KCP session conv={} peer={}", conv, peer_id);
    return session;
}

void KcpTransport::DestroySession(KcpSession* session) {
    SPDLOG_DEBUG("Destroying KCP session conv={} peer={}", session->conv, session->peer_id);

    sessions_by_conv_.erase(session->conv);
    if (session->peer_id != 0) {
        sessions_by_peer_.erase(session->peer_id);
        peer_manager_.UpdateState(session->peer_id, PeerState::Disconnected);
    }
    AddrKey key{session->remote_addr};
    sessions_by_addr_.erase(key);

    delete session;
}

void KcpTransport::InitiateHandshake(uint64_t peer_id) {
    auto* peer = peer_manager_.GetPeer(peer_id);
    if (!peer || peer->state != PeerState::Disconnected) {
        return;
    }

    auto it = peer_addresses_.find(peer_id);
    if (it == peer_addresses_.end()) {
        SPDLOG_WARN("No address for peer {}", peer_id);
        return;
    }

    uint32_t conv = AllocateConvId();

    KcpHandshakePacket pkt{};
    pkt.magic = kKcpHandshakeMagic;
    pkt.type = static_cast<uint8_t>(KcpHandshakeType::Request);
    pkt.conv = conv;
    pkt.node_id = config_.node_id;

    auto buf = pkt.Encode();
    SendUdp(
        buf.data(), buf.size(), reinterpret_cast<const sockaddr*>(&it->second), sizeof(sockaddr_in)
    );

    peer_manager_.UpdateState(peer_id, PeerState::Connecting);
    SPDLOG_DEBUG("Initiating KCP handshake to peer {} with conv {}", peer_id, conv);
}

void KcpTransport::HandleKcpHandshake(
    const char* data, size_t len, const sockaddr* addr, socklen_t addr_len
) {
    auto result = KcpHandshakePacket::Decode(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(data), len)
    );
    if (!result) {
        SPDLOG_WARN("Invalid KCP handshake: {}", result.error().ToString());
        return;
    }

    const auto& pkt = *result;
    auto type = static_cast<KcpHandshakeType>(pkt.type);

    sockaddr_storage storage{};
    std::memcpy(&storage, addr, addr_len);

    if (type == KcpHandshakeType::Request) {
        uint64_t remote_id = pkt.node_id;

        // Check for simultaneous handshake: are we also trying to connect to them?
        auto* peer = peer_manager_.GetPeer(remote_id);
        if (peer && peer->state == PeerState::Connecting) {
            // Simultaneous handshake detected
            // Use node_id comparison: lower node_id becomes the initiator
            if (config_.node_id < remote_id) {
                // We should be the initiator, ignore their Request
                SPDLOG_DEBUG(
                    "Simultaneous handshake with {}: we are initiator, ignoring request", remote_id
                );
                return;
            }
            // We should be the acceptor, continue with normal Request handling
            SPDLOG_DEBUG("Simultaneous handshake with {}: we are acceptor", remote_id);
        }

        uint32_t their_conv = pkt.conv;
        uint32_t our_index = AllocateConvId();
        // Combine both sides' conv values: initiator's conv in upper 16 bits, acceptor's in lower
        uint32_t final_conv = ((their_conv & 0xFFFF) << 16) | (our_index & 0xFFFF);

        KcpHandshakePacket response{};
        response.magic = kKcpHandshakeMagic;
        response.type = static_cast<uint8_t>(KcpHandshakeType::Response);
        response.conv = final_conv;
        response.node_id = config_.node_id;

        auto buf = response.Encode();
        SendUdp(buf.data(), buf.size(), addr, addr_len);

        auto* session = CreateSession(final_conv, remote_id, storage, addr_len, false);
        peer_manager_.UpdateState(remote_id, PeerState::Connecting);

        SendAppHandshake(session);

        SPDLOG_DEBUG("Accepted KCP handshake from {} with conv {}", remote_id, final_conv);

    } else if (type == KcpHandshakeType::Response) {
        uint32_t final_conv = pkt.conv;
        uint64_t remote_id = pkt.node_id;

        // Check if we already have a session with this peer (from simultaneous handshake)
        if (auto it = sessions_by_peer_.find(remote_id); it != sessions_by_peer_.end()) {
            // Simultaneous handshake - we created a session when we received their Request
            // Use node_id comparison: lower node_id becomes the initiator
            if (config_.node_id > remote_id) {
                // We should be the acceptor, keep the existing session
                SPDLOG_DEBUG("Simultaneous handshake with {}: keeping acceptor session", remote_id);
                return;
            }
            // We should be the initiator, destroy the acceptor session
            SPDLOG_DEBUG(
                "Simultaneous handshake with {}: switching to initiator session", remote_id
            );
            DestroySession(it->second);
        } else {
            // Check by address for sessions without peer_id set yet
            AddrKey key{storage};
            if (auto it2 = sessions_by_addr_.find(key); it2 != sessions_by_addr_.end()) {
                DestroySession(it2->second);
            }
        }

        auto* session = CreateSession(final_conv, remote_id, storage, addr_len, true);

        SendAppHandshake(session);

        SPDLOG_DEBUG("KCP handshake response from {} with conv {}", remote_id, final_conv);
    }
}

void KcpTransport::SendAppHandshake(KcpSession* session) {
    Handshake hs;
    hs.node_id = config_.node_id;
    auto buf = hs.Encode();
    ikcp_send(
        session->kcp, reinterpret_cast<const char*>(buf.data()), static_cast<int>(buf.size())
    );
}

void KcpTransport::OnAppHandshakeReceived(KcpSession* session, uint64_t remote_id) {
    if (session->is_initiator) {
        // For outgoing connections, we already know the expected peer_id
        if (session->peer_id != 0 && session->peer_id != remote_id) {
            SPDLOG_WARN("Peer ID mismatch: expected {}, got {}", session->peer_id, remote_id);
        }
    } else {
        // For incoming connections, update peer_id from handshake
        if (session->peer_id != remote_id) {
            sessions_by_peer_.erase(session->peer_id);
            session->peer_id = remote_id;
            sessions_by_peer_[remote_id] = session;
        }
    }

    session->handshake_done = true;
    peer_manager_.UpdateState(session->peer_id, PeerState::Connected);
    SPDLOG_DEBUG("App handshake complete with peer {}", session->peer_id);
}

void KcpTransport::ProcessUdpPacket(
    const char* data, size_t len, const sockaddr* addr, socklen_t addr_len
) {
    if (len < 4) {
        return;
    }

    uint32_t magic;
    std::memcpy(&magic, data, 4);
    if (magic == kKcpHandshakeMagic) {
        HandleKcpHandshake(data, len, addr, addr_len);
        return;
    }

    uint32_t conv = ikcp_getconv(data);

    auto it = sessions_by_conv_.find(conv);
    if (it == sessions_by_conv_.end()) {
        return;
    }

    KcpSession* session = it->second;
    session->last_activity = std::chrono::steady_clock::now();

    int ret = ikcp_input(session->kcp, data, static_cast<long>(len));
    if (ret < 0) {
        SPDLOG_WARN("KCP input error: {}", ret);
        return;
    }

    TryReceive(session);
}

void KcpTransport::TryReceive(KcpSession* session) {
    while (true) {
        int peek_size = ikcp_peeksize(session->kcp);
        if (peek_size < 0) {
            break;
        }

        session->recv_buf.resize(peek_size);
        int received =
            ikcp_recv(session->kcp, reinterpret_cast<char*>(session->recv_buf.data()), peek_size);
        if (received < 0) {
            break;
        }

        if (!ProcessReceivedData(session)) {
            // Session was destroyed during processing
            return;
        }
    }
}

bool KcpTransport::ProcessReceivedData(KcpSession* session) {
    auto& buf = session->recv_buf;

    if (!session->handshake_done) {
        if (buf.size() >= Handshake::kSize) {
            auto hs_result = Handshake::Decode(buf);
            if (!hs_result) {
                SPDLOG_WARN("Invalid app handshake: {}", hs_result.error().ToString());
                DestroySession(session);
                return false;
            }

            OnAppHandshakeReceived(session, hs_result->node_id);
            buf.erase(buf.begin(), buf.begin() + Handshake::kSize);
            // Note: both sides send app handshake in HandleKcpHandshake when creating the session,
            // so no need to send another one here.
        }
        return true;
    }

    while (true) {
        auto frame_result = Codec::FrameSize(buf, config_.max_message_size);
        if (!frame_result) {
            SPDLOG_WARN("Invalid frame: {}", frame_result.error().ToString());
            DestroySession(session);
            return false;
        }

        size_t frame_size = *frame_result;
        if (frame_size == 0 || buf.size() < frame_size) {
            break;
        }

        auto decode_result = Codec::Decode(buf, config_.max_message_size);
        if (!decode_result) {
            SPDLOG_WARN("Decode failed: {}", decode_result.error().ToString());
            DestroySession(session);
            return false;
        }

        auto [header, msg, consumed] = std::move(*decode_result);
        if (consumed == 0) {
            break;
        }

        buf.erase(buf.begin(), buf.begin() + consumed);
        peer_manager_.RecordActivity(session->peer_id);

        if (on_message_) {
            on_message_(std::move(msg));
        }
    }
    return true;
}

void KcpTransport::SendUdp(
    const void* data, size_t len, const sockaddr* addr, socklen_t /*addr_len*/
) {
    auto* req = new uv_udp_send_t();
    auto* buf_data =
        new std::vector<char>(static_cast<const char*>(data), static_cast<const char*>(data) + len);
    req->data = buf_data;

    uv_buf_t buf = uv_buf_init(buf_data->data(), buf_data->size());

    uv_udp_send(req, &udp_handle_, &buf, 1, addr, [](uv_udp_send_t* req, int status) {
        auto* buf_data = static_cast<std::vector<char>*>(req->data);
        delete buf_data;
        delete req;
        if (status != 0) {
            SPDLOG_DEBUG("UDP send failed: {}", uv_strerror(status));
        }
    });
}

void KcpTransport::UpdateAllKcpSessions() {
    uint32_t current = GetCurrentMs();
    auto now = std::chrono::steady_clock::now();

    std::vector<KcpSession*> to_remove;

    for (auto& [conv, session] : sessions_by_conv_) {
        if (!session->kcp) {
            continue;
        }

        ikcp_update(session->kcp, current);

        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - session->last_activity)
                .count();
        if (static_cast<uint32_t>(elapsed) > kcp_config_.session_timeout_ms) {
            to_remove.push_back(session);
        }
    }

    for (auto* session : to_remove) {
        SPDLOG_DEBUG("Session timeout for peer {}", session->peer_id);
        if (session->peer_id != 0) {
            peer_manager_.RecordFailure(session->peer_id);
            if (on_error_) {
                on_error_(session->peer_id, "session timeout");
            }
        }
        DestroySession(session);
    }
}

// Static callbacks
int KcpTransport::KcpOutput(const char* buf, int len, ikcpcb* /*kcp*/, void* user) {
    auto* session = static_cast<KcpSession*>(user);
    auto* transport = static_cast<KcpTransport*>(session->transport);
    transport->SendUdp(
        buf, len, reinterpret_cast<const sockaddr*>(&session->remote_addr), session->addr_len
    );
    return 0;
}

void KcpTransport::OnAlloc(uv_handle_t* /*handle*/, size_t suggested_size, uv_buf_t* buf) {
    buf->base = new char[suggested_size];
    buf->len = suggested_size;
}

void KcpTransport::OnRecv(
    uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, const sockaddr* addr, unsigned /*flags*/
) {
    auto* transport = static_cast<KcpTransport*>(handle->data);

    if (nread < 0 || addr == nullptr) {
        delete[] buf->base;
        return;
    }

    if (nread > 0) {
        socklen_t addr_len =
            (addr->sa_family == AF_INET) ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
        transport->ProcessUdpPacket(buf->base, static_cast<size_t>(nread), addr, addr_len);
    }

    delete[] buf->base;
}

void KcpTransport::OnKcpUpdate(uv_timer_t* timer) {
    auto* transport = static_cast<KcpTransport*>(timer->data);
    if (transport->stopped_) {
        return;
    }
    transport->UpdateAllKcpSessions();
}

void KcpTransport::OnReconnectTimer(uv_timer_t* timer) {
    auto* transport = static_cast<KcpTransport*>(timer->data);
    if (transport->stopped_) {
        return;
    }

    for (auto peer_id : transport->peer_manager_.GetPeersToReconnect()) {
        transport->InitiateHandshake(peer_id);
    }
}

}  // namespace raftpp::rpc
