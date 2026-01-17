#include "raftpp/rpc/kcp_transport.h"

#include <cstring>

#include <spdlog/spdlog.h>
#include <uv.h>

extern "C" {
#include "ikcp.h"
}

#include "raftpp/rpc/peer_manager.h"

namespace raftpp::rpc {

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

    std::vector<uint8_t> Encode() const {
        std::vector<uint8_t> buf(kSize);
        std::memcpy(buf.data(), &magic, 4);
        buf[4] = type;
        std::memcpy(buf.data() + 5, &conv, 4);
        std::memcpy(buf.data() + 9, &node_id, 8);
        return buf;
    }

    static RpcResult<KcpHandshakePacket> Decode(std::span<const uint8_t> buf) {
        if (buf.size() < kSize) {
            return std::unexpected(RpcError::InvalidMessage("KCP handshake too short"));
        }

        KcpHandshakePacket pkt;
        std::memcpy(&pkt.magic, buf.data(), 4);
        pkt.type = buf[4];
        std::memcpy(&pkt.conv, buf.data() + 5, 4);
        std::memcpy(&pkt.node_id, buf.data() + 9, 8);

        if (pkt.magic != kKcpHandshakeMagic) {
            return std::unexpected(RpcError::InvalidMessage("Invalid KCP handshake magic"));
        }

        return pkt;
    }
};

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

/// Get current time in milliseconds
uint32_t GetCurrentMs() {
    using namespace std::chrono;
    auto now = steady_clock::now();
    return static_cast<uint32_t>(duration_cast<milliseconds>(now.time_since_epoch()).count());
}

/// Compare two sockaddr structures
bool SockaddrEqual(const sockaddr_storage& a, const sockaddr_storage& b) {
    if (a.ss_family != b.ss_family) {
        return false;
    }

    if (a.ss_family == AF_INET) {
        const auto* a4 = reinterpret_cast<const sockaddr_in*>(&a);
        const auto* b4 = reinterpret_cast<const sockaddr_in*>(&b);
        return a4->sin_port == b4->sin_port &&
               a4->sin_addr.s_addr == b4->sin_addr.s_addr;
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
size_t HashSockaddr(const sockaddr_storage& addr) {
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

/// Address key for hash map
struct AddrKey {
    sockaddr_storage addr{};

    bool operator==(const AddrKey& other) const { return SockaddrEqual(addr, other.addr); }
};

struct AddrKeyHash {
    size_t operator()(const AddrKey& k) const { return HashSockaddr(k.addr); }
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

    ~KcpSession() {
        if (kcp) {
            ikcp_release(kcp);
            kcp = nullptr;
        }
    }
};

struct KcpTransport::Impl {
    TransportConfig config;
    KcpConfig kcp_config;

    uv_loop_t loop{};
    uv_udp_t udp_handle{};
    uv_timer_t update_timer{};
    uv_timer_t reconnect_timer{};

    PeerManager peer_manager;
    Map<uint64_t, KcpSession*> sessions_by_peer;
    Map<uint32_t, KcpSession*> sessions_by_conv;
    absl::flat_hash_map<AddrKey, KcpSession*, AddrKeyHash> sessions_by_addr;
    Map<uint64_t, sockaddr_storage> peer_addresses;

    uint32_t next_conv_id = 1;

    MessageCallback on_message;
    ErrorCallback on_error;

    bool running = false;
    bool stopped = false;

    Impl(TransportConfig cfg, KcpConfig kcp_cfg)
        : config(std::move(cfg)), kcp_config(kcp_cfg) {
        uv_loop_init(&loop);
        udp_handle.data = this;
        update_timer.data = this;
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

        auto addr_result = ParseAddress(config.listen_addr);
        if (!addr_result) {
            return std::unexpected(addr_result.error());
        }
        auto [host, port] = *addr_result;

        uv_udp_init(&loop, &udp_handle);

        sockaddr_in addr{};
        uv_ip4_addr(host.c_str(), port, &addr);

        int r = uv_udp_bind(&udp_handle, reinterpret_cast<const sockaddr*>(&addr), 0);
        if (r != 0) {
            return std::unexpected(
                RpcError::ConnectionFailed(std::string("UDP bind failed: ") + uv_strerror(r)));
        }

        r = uv_udp_recv_start(&udp_handle, OnAlloc, OnRecv);
        if (r != 0) {
            return std::unexpected(
                RpcError::ConnectionFailed(std::string("UDP recv start failed: ") + uv_strerror(r)));
        }

        uv_timer_init(&loop, &update_timer);
        uv_timer_start(&update_timer, OnKcpUpdate, kcp_config.interval, kcp_config.interval);

        uv_timer_init(&loop, &reconnect_timer);
        uv_timer_start(&reconnect_timer, OnReconnectTimer, 1000, 1000);

        running = true;
        stopped = false;

        SPDLOG_INFO("KCP Transport started on {}", config.listen_addr);

        for (auto peer_id : peer_manager.GetAllPeerIds()) {
            InitiateHandshake(peer_id);
        }

        return {};
    }

    void Stop() {
        if (!running || stopped) {
            return;
        }
        stopped = true;

        for (auto& [_, session] : sessions_by_peer) {
            delete session;
        }
        sessions_by_peer.clear();
        sessions_by_conv.clear();
        sessions_by_addr.clear();

        uv_timer_stop(&update_timer);
        uv_close(reinterpret_cast<uv_handle_t*>(&update_timer), nullptr);

        uv_timer_stop(&reconnect_timer);
        uv_close(reinterpret_cast<uv_handle_t*>(&reconnect_timer), nullptr);

        uv_udp_recv_stop(&udp_handle);
        uv_close(reinterpret_cast<uv_handle_t*>(&udp_handle), nullptr);

        while (uv_loop_alive(&loop)) {
            uv_run(&loop, UV_RUN_ONCE);
        }

        running = false;
        SPDLOG_INFO("KCP Transport stopped");
    }

    void AddPeer(uint64_t id, const std::string& addr_str) {
        peer_manager.AddPeer(id, addr_str);

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
        peer_addresses[id] = storage;

        if (running) {
            InitiateHandshake(id);
        }
    }

    void RemovePeer(uint64_t id) {
        peer_manager.RemovePeer(id);
        peer_addresses.erase(id);

        if (auto it = sessions_by_peer.find(id); it != sessions_by_peer.end()) {
            DestroySession(it->second);
        }
    }

    void Send(std::span<const Message> messages) {
        for (const auto& msg : messages) {
            uint64_t to = msg.to();
            auto it = sessions_by_peer.find(to);
            if (it == sessions_by_peer.end() || !it->second->handshake_done) {
                SPDLOG_DEBUG("Dropping message to {}: not connected", to);
                continue;
            }

            auto buf = Codec::Encode(msg);
            int ret = ikcp_send(it->second->kcp, reinterpret_cast<const char*>(buf.data()),
                                static_cast<int>(buf.size()));
            if (ret < 0) {
                SPDLOG_WARN("KCP send failed to {}: {}", to, ret);
            }
        }
    }

    void Poll(std::chrono::milliseconds timeout) {
        uv_run(&loop, UV_RUN_NOWAIT);

        if (timeout.count() > 0) {
            uv_timer_t timer;
            timer.data = &loop;
            uv_timer_init(&loop, &timer);
            uv_timer_start(
                &timer,
                [](uv_timer_t* t) { uv_stop(static_cast<uv_loop_t*>(t->data)); },
                timeout.count(),
                0);

            uv_run(&loop, UV_RUN_DEFAULT);

            uv_timer_stop(&timer);
            uv_close(reinterpret_cast<uv_handle_t*>(&timer), nullptr);
            uv_run(&loop, UV_RUN_NOWAIT);
        }
    }

    void Run() { uv_run(&loop, UV_RUN_DEFAULT); }

    uint32_t AllocateConvId() {
        while (sessions_by_conv.contains(next_conv_id)) {
            next_conv_id++;
            if (next_conv_id == 0) {
                next_conv_id = 1;
            }
        }
        return next_conv_id++;
    }

    KcpSession* CreateSession(uint32_t conv, uint64_t peer_id, const sockaddr_storage& addr,
                              socklen_t addr_len, bool is_initiator) {
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
        ikcp_nodelay(session->kcp, kcp_config.nodelay, kcp_config.interval, kcp_config.resend,
                     kcp_config.nc);
        ikcp_wndsize(session->kcp, kcp_config.snd_wnd, kcp_config.rcv_wnd);
        ikcp_setmtu(session->kcp, kcp_config.mtu);

        sessions_by_conv[conv] = session;
        if (peer_id != 0) {
            sessions_by_peer[peer_id] = session;
        }
        AddrKey key{addr};
        sessions_by_addr[key] = session;

        SPDLOG_DEBUG("Created KCP session conv={} peer={}", conv, peer_id);
        return session;
    }

    void DestroySession(KcpSession* session) {
        SPDLOG_DEBUG("Destroying KCP session conv={} peer={}", session->conv, session->peer_id);

        sessions_by_conv.erase(session->conv);
        if (session->peer_id != 0) {
            sessions_by_peer.erase(session->peer_id);
            peer_manager.UpdateState(session->peer_id, PeerState::Disconnected);
        }
        AddrKey key{session->remote_addr};
        sessions_by_addr.erase(key);

        delete session;
    }

    void InitiateHandshake(uint64_t peer_id) {
        auto* peer = peer_manager.GetPeer(peer_id);
        if (!peer || peer->state != PeerState::Disconnected) {
            return;
        }

        auto it = peer_addresses.find(peer_id);
        if (it == peer_addresses.end()) {
            SPDLOG_WARN("No address for peer {}", peer_id);
            return;
        }

        uint32_t conv = AllocateConvId();

        KcpHandshakePacket pkt{};
        pkt.magic = kKcpHandshakeMagic;
        pkt.type = static_cast<uint8_t>(KcpHandshakeType::Request);
        pkt.conv = conv;
        pkt.node_id = config.node_id;

        auto buf = pkt.Encode();
        SendUdp(buf.data(), buf.size(), reinterpret_cast<const sockaddr*>(&it->second),
                sizeof(sockaddr_in));

        peer_manager.UpdateState(peer_id, PeerState::Connecting);
        SPDLOG_DEBUG("Initiating KCP handshake to peer {} with conv {}", peer_id, conv);
    }

    void HandleKcpHandshake(const char* data, size_t len, const sockaddr* addr,
                            socklen_t addr_len) {
        auto result = KcpHandshakePacket::Decode(
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(data), len));
        if (!result) {
            SPDLOG_WARN("Invalid KCP handshake: {}", result.error().ToString());
            return;
        }

        const auto& pkt = *result;
        auto type = static_cast<KcpHandshakeType>(pkt.type);

        sockaddr_storage storage{};
        std::memcpy(&storage, addr, addr_len);

        if (type == KcpHandshakeType::Request) {
            uint32_t their_conv = pkt.conv;
            uint32_t our_index = AllocateConvId();
            uint32_t final_conv = (their_conv & 0xFFFF0000) | (our_index & 0x0000FFFF);

            KcpHandshakePacket response{};
            response.magic = kKcpHandshakeMagic;
            response.type = static_cast<uint8_t>(KcpHandshakeType::Response);
            response.conv = final_conv;
            response.node_id = config.node_id;

            auto buf = response.Encode();
            SendUdp(buf.data(), buf.size(), addr, addr_len);

            auto* session = CreateSession(final_conv, pkt.node_id, storage, addr_len, false);
            peer_manager.UpdateState(pkt.node_id, PeerState::Connecting);

            SendAppHandshake(session);

            SPDLOG_DEBUG("Accepted KCP handshake from {} with conv {}", pkt.node_id, final_conv);

        } else if (type == KcpHandshakeType::Response) {
            uint32_t final_conv = pkt.conv;
            uint64_t remote_id = pkt.node_id;

            AddrKey key{storage};
            if (auto it = sessions_by_addr.find(key); it != sessions_by_addr.end()) {
                DestroySession(it->second);
            }

            auto* session = CreateSession(final_conv, remote_id, storage, addr_len, true);

            SendAppHandshake(session);

            SPDLOG_DEBUG("KCP handshake response from {} with conv {}", remote_id, final_conv);
        }
    }

    void SendAppHandshake(KcpSession* session) {
        Handshake hs;
        hs.node_id = config.node_id;
        auto buf = hs.Encode();
        ikcp_send(session->kcp, reinterpret_cast<const char*>(buf.data()),
                  static_cast<int>(buf.size()));
    }

    void OnAppHandshakeReceived(KcpSession* session, uint64_t remote_id) {
        if (session->is_initiator) {
            if (session->peer_id != 0 && session->peer_id != remote_id) {
                SPDLOG_WARN("Peer ID mismatch: expected {}, got {}", session->peer_id, remote_id);
            }
        } else {
            if (session->peer_id != remote_id) {
                sessions_by_peer.erase(session->peer_id);
                session->peer_id = remote_id;
                sessions_by_peer[remote_id] = session;
            }
        }

        session->handshake_done = true;
        peer_manager.UpdateState(session->peer_id, PeerState::Connected);
        SPDLOG_DEBUG("App handshake complete with peer {}", session->peer_id);
    }

    void ProcessUdpPacket(const char* data, size_t len, const sockaddr* addr, socklen_t addr_len) {
        if (len >= 4) {
            uint32_t magic;
            std::memcpy(&magic, data, 4);
            if (magic == kKcpHandshakeMagic) {
                HandleKcpHandshake(data, len, addr, addr_len);
                return;
            }
        }

        if (len < 4) {
            return;
        }
        uint32_t conv = ikcp_getconv(data);

        auto it = sessions_by_conv.find(conv);
        if (it == sessions_by_conv.end()) {
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

    void TryReceive(KcpSession* session) {
        while (true) {
            int peek_size = ikcp_peeksize(session->kcp);
            if (peek_size < 0) {
                break;
            }

            session->recv_buf.resize(peek_size);
            int received = ikcp_recv(session->kcp,
                                     reinterpret_cast<char*>(session->recv_buf.data()), peek_size);
            if (received < 0) {
                break;
            }

            ProcessReceivedData(session);
        }
    }

    void ProcessReceivedData(KcpSession* session) {
        auto& buf = session->recv_buf;

        if (!session->handshake_done) {
            if (buf.size() >= Handshake::kSize) {
                auto hs_result = Handshake::Decode(buf);
                if (!hs_result) {
                    SPDLOG_WARN("Invalid app handshake: {}", hs_result.error().ToString());
                    DestroySession(session);
                    return;
                }

                OnAppHandshakeReceived(session, hs_result->node_id);
                buf.erase(buf.begin(), buf.begin() + Handshake::kSize);

                if (!session->is_initiator) {
                    SendAppHandshake(session);
                }
            }
            return;
        }

        while (true) {
            auto frame_result = Codec::FrameSize(buf, config.max_message_size);
            if (!frame_result) {
                SPDLOG_WARN("Invalid frame: {}", frame_result.error().ToString());
                DestroySession(session);
                return;
            }

            size_t frame_size = *frame_result;
            if (frame_size == 0 || buf.size() < frame_size) {
                break;
            }

            auto decode_result = Codec::Decode(buf, config.max_message_size);
            if (!decode_result) {
                SPDLOG_WARN("Decode failed: {}", decode_result.error().ToString());
                DestroySession(session);
                return;
            }

            auto [header, msg, consumed] = std::move(*decode_result);
            if (consumed == 0) {
                break;
            }

            buf.erase(buf.begin(), buf.begin() + consumed);
            peer_manager.RecordActivity(session->peer_id);

            if (on_message) {
                on_message(std::move(msg));
            }
        }
    }

    void SendUdp(const void* data, size_t len, const sockaddr* addr, socklen_t /*addr_len*/) {
        auto* req = new uv_udp_send_t();
        auto* buf_data = new std::vector<char>(static_cast<const char*>(data),
                                               static_cast<const char*>(data) + len);
        req->data = buf_data;

        uv_buf_t buf = uv_buf_init(buf_data->data(), buf_data->size());

        uv_udp_send(
            req, &udp_handle, &buf, 1, addr, [](uv_udp_send_t* req, int status) {
                auto* buf_data = static_cast<std::vector<char>*>(req->data);
                delete buf_data;
                delete req;
                if (status != 0) {
                    SPDLOG_DEBUG("UDP send failed: {}", uv_strerror(status));
                }
            });
    }

    void UpdateAllKcpSessions() {
        uint32_t current = GetCurrentMs();
        auto now = std::chrono::steady_clock::now();

        std::vector<KcpSession*> to_remove;

        for (auto& [conv, session] : sessions_by_conv) {
            if (!session->kcp) {
                continue;
            }

            ikcp_update(session->kcp, current);

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               now - session->last_activity)
                               .count();
            if (static_cast<uint32_t>(elapsed) > kcp_config.session_timeout_ms) {
                to_remove.push_back(session);
            }
        }

        for (auto* session : to_remove) {
            SPDLOG_DEBUG("Session timeout for peer {}", session->peer_id);
            if (session->peer_id != 0) {
                peer_manager.RecordFailure(session->peer_id);
                if (on_error) {
                    on_error(session->peer_id, "session timeout");
                }
            }
            DestroySession(session);
        }
    }

    static int KcpOutput(const char* buf, int len, ikcpcb* /*kcp*/, void* user) {
        auto* session = static_cast<KcpSession*>(user);
        auto* impl = static_cast<Impl*>(session->transport);
        impl->SendUdp(buf, len, reinterpret_cast<const sockaddr*>(&session->remote_addr),
                      session->addr_len);
        return 0;
    }

    static void OnAlloc(uv_handle_t* /*handle*/, size_t suggested_size, uv_buf_t* buf) {
        buf->base = new char[suggested_size];
        buf->len = suggested_size;
    }

    static void OnRecv(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, const sockaddr* addr,
                       unsigned /*flags*/) {
        auto* impl = static_cast<Impl*>(handle->data);

        if (nread < 0 || addr == nullptr) {
            delete[] buf->base;
            return;
        }

        if (nread > 0) {
            socklen_t addr_len = (addr->sa_family == AF_INET) ? sizeof(sockaddr_in)
                                                              : sizeof(sockaddr_in6);
            impl->ProcessUdpPacket(buf->base, static_cast<size_t>(nread), addr, addr_len);
        }

        delete[] buf->base;
    }

    static void OnKcpUpdate(uv_timer_t* timer) {
        auto* impl = static_cast<Impl*>(timer->data);
        if (impl->stopped) {
            return;
        }
        impl->UpdateAllKcpSessions();
    }

    static void OnReconnectTimer(uv_timer_t* timer) {
        auto* impl = static_cast<Impl*>(timer->data);
        if (impl->stopped) {
            return;
        }

        for (auto peer_id : impl->peer_manager.GetPeersToReconnect()) {
            impl->InitiateHandshake(peer_id);
        }
    }
};

KcpTransport::KcpTransport(TransportConfig config, KcpConfig kcp_config)
    : impl_(std::make_unique<Impl>(std::move(config), kcp_config)) {}

KcpTransport::~KcpTransport() = default;

RpcResult<void> KcpTransport::Start() { return impl_->Start(); }

void KcpTransport::Stop() { impl_->Stop(); }

void KcpTransport::AddPeer(uint64_t id, const std::string& addr) { impl_->AddPeer(id, addr); }

void KcpTransport::RemovePeer(uint64_t id) { impl_->RemovePeer(id); }

void KcpTransport::Send(std::span<const Message> messages) { impl_->Send(messages); }

void KcpTransport::SetMessageCallback(MessageCallback cb) { impl_->on_message = std::move(cb); }

void KcpTransport::SetErrorCallback(ErrorCallback cb) { impl_->on_error = std::move(cb); }

void KcpTransport::Poll(std::chrono::milliseconds timeout) { impl_->Poll(timeout); }

void KcpTransport::Run() { impl_->Run(); }

}  // namespace raftpp::rpc
