#include "harness/network.h"

namespace raftpp {

Network Network::CreateWithConfig(size_t num_peers, const Config& config) {
    std::unordered_map<uint64_t, Interface> peers;
    std::unordered_map<uint64_t, MemoryStorage*> storage;

    for (size_t i = 0; i < num_peers; ++i) {
        const uint64_t id = static_cast<uint64_t>(i + 1);
        Snapshot snap;
        snap.mutable_metadata()->set_index(1);
        snap.mutable_metadata()->set_term(1);
        snap.mutable_metadata()->mutable_conf_state()->mutable_voters()->Add(id);

        auto store = new MemoryStorage();
        store->ApplySnapshot(snap);

        Config node_config = config;
        node_config.id = id;

        auto raft = std::make_unique<Raft>(node_config, std::unique_ptr<Storage>(store));
        peers.emplace(id, Interface(std::move(raft)));
        storage.emplace(id, store);
    }

    return Network(peers, storage);
}

Interface* Network::GetPeer(uint64_t id) {
    auto it = peers_.find(id);
    if (it == peers_.end()) {
        return nullptr;
    }
    return &it->second;
}

const Interface* Network::GetPeer(uint64_t id) const {
    auto it = peers_.find(id);
    if (it == peers_.end()) {
        return nullptr;
    }
    return &it->second;
}

MemoryStorage* Network::GetStorage(uint64_t id) {
    auto it = storage_.find(id);
    if (it == storage_.end()) {
        return nullptr;
    }
    return it->second;
}

const MemoryStorage* Network::GetStorage(uint64_t id) const {
    auto it = storage_.find(id);
    if (it == storage_.end()) {
        return nullptr;
    }
    return it->second;
}

void Network::IgnoreMessageType(MessageType type) {
    ignore_message_types_[static_cast<uint64_t>(type)] = true;
}

std::vector<Message> Network::Filter(const std::vector<Message>& msgs) {
    std::vector<Message> filtered;
    filtered.reserve(msgs.size());

    for (const auto& m : msgs) {
        const auto msg_type = static_cast<uint64_t>(m.msg_type());

        // Check if message type is ignored
        if (ignore_message_types_.count(msg_type)) {
            continue;
        }

        // Hup messages never go over network
        if (m.msg_type() == MsgHup) {
            continue;
        }

        // Check drop rate
        Connection conn{m.from(), m.to()};
        auto drop_it = drop_rates_.find(conn);
        if (drop_it != drop_rates_.end()) {
            const double drop_chance = drop_it->second;
            // Simple random implementation
            if ((static_cast<double>(rand()) / RAND_MAX) < drop_chance) {
                continue;
            }
        }

        filtered.push_back(m);
    }

    return filtered;
}

std::vector<Message> Network::ReadMessages() {
    std::vector<Message> all_msgs;

    for (auto& [id, peer] : peers_) {
        auto msgs = peer.ReadMessages();
        all_msgs.insert(all_msgs.end(), msgs.begin(), msgs.end());
    }

    return all_msgs;
}

void Network::Send(std::vector<Message> msgs) {
    while (!msgs.empty()) {
        std::vector<Message> new_msgs;

        for (auto& m : msgs) {
            Interface* peer = GetPeer(m.to());
            if (!peer || !peer->HasRaft()) {
                continue;
            }

            auto result = peer->Step(m);
            // Ignore errors for now
            (void)result;

            peer->Persist();

            auto resp = peer->ReadMessages();
            new_msgs.insert(new_msgs.end(), resp.begin(), resp.end());
        }

        msgs = Filter(new_msgs);
    }
}

void Network::FilterAndSend(std::vector<Message> msgs) {
    Send(Filter(msgs));
}

Result<void> Network::Dispatch(const std::vector<Message>& messages) {
    for (const auto& m : Filter(messages)) {
        Interface* peer = GetPeer(m.to());
        if (!peer || !peer->HasRaft()) {
            continue;
        }
        auto result = peer->Step(m);
        if (!result) {
            return result.error();
        }
    }
    return {};
}

void Network::Drop(uint64_t from, uint64_t to, double percentage) {
    drop_rates_[Connection{from, to}] = percentage;
}

void Network::Cut(uint64_t one, uint64_t other) {
    Drop(one, other, 1.0);
    Drop(other, one, 1.0);
}

void Network::Isolate(uint64_t id) {
    for (const auto& [peer_id, _] : peers_) {
        if (peer_id != id) {
            Drop(id, peer_id, 1.0);
            Drop(peer_id, id, 1.0);
        }
    }
}

void Network::Recover() {
    ignore_message_types_.clear();
    drop_rates_.clear();
}

}  // namespace raftpp
