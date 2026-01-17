#include "raftpp/rpc/peer_manager.h"

#include <algorithm>

namespace raftpp::rpc {

void PeerManager::AddPeer(uint64_t id, std::string addr) {
    PeerInfo info;
    info.id = id;
    info.addr = std::move(addr);
    info.state = PeerState::Disconnected;
    info.last_activity = std::chrono::steady_clock::now();
    info.failure_count = 0;
    info.reconnect_after = std::chrono::steady_clock::time_point{};

    peers_[id] = std::move(info);
}

void PeerManager::RemovePeer(uint64_t id) {
    peers_.erase(id);
}

PeerInfo* PeerManager::GetPeer(uint64_t id) {
    auto it = peers_.find(id);
    return it != peers_.end() ? &it->second : nullptr;
}

const PeerInfo* PeerManager::GetPeer(uint64_t id) const {
    auto it = peers_.find(id);
    return it != peers_.end() ? &it->second : nullptr;
}

std::vector<uint64_t> PeerManager::GetAllPeerIds() const {
    std::vector<uint64_t> ids;
    ids.reserve(peers_.size());
    for (const auto& [id, _] : peers_) {
        ids.push_back(id);
    }
    return ids;
}

std::vector<uint64_t> PeerManager::GetPeersToReconnect() const {
    std::vector<uint64_t> ids;
    auto now = std::chrono::steady_clock::now();

    for (const auto& [id, info] : peers_) {
        if (info.state == PeerState::Disconnected && now >= info.reconnect_after) {
            ids.push_back(id);
        }
    }
    return ids;
}

void PeerManager::UpdateState(uint64_t id, PeerState state) {
    if (auto* peer = GetPeer(id)) {
        peer->state = state;
        if (state == PeerState::Connected) {
            // Reset failure count on successful connection
            peer->failure_count = 0;
            peer->reconnect_after = std::chrono::steady_clock::time_point{};
        }
    }
}

void PeerManager::RecordFailure(uint64_t id) {
    if (auto* peer = GetPeer(id)) {
        peer->state = PeerState::Disconnected;
        peer->failure_count++;
        peer->reconnect_after =
            std::chrono::steady_clock::now() + CalculateBackoff(peer->failure_count);
    }
}

void PeerManager::RecordActivity(uint64_t id) {
    if (auto* peer = GetPeer(id)) {
        peer->last_activity = std::chrono::steady_clock::now();
    }
}

bool PeerManager::HasPeer(uint64_t id) const {
    return peers_.contains(id);
}

size_t PeerManager::ConnectedCount() const {
    size_t count = 0;
    for (const auto& [_, info] : peers_) {
        if (info.state == PeerState::Connected) {
            count++;
        }
    }
    return count;
}

std::chrono::milliseconds PeerManager::CalculateBackoff(uint32_t failure_count) {
    // Exponential backoff: 1s, 2s, 4s, 8s, 16s, capped at 30s
    constexpr uint32_t kBaseMs = 1000;
    constexpr uint32_t kMaxMs = 30000;

    uint32_t delay_ms = kBaseMs << std::min(failure_count - 1, 4u);
    return std::chrono::milliseconds(std::min(delay_ms, kMaxMs));
}

}  // namespace raftpp::rpc
