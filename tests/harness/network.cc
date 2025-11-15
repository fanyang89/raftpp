#include "harness/network.h"

#include <cassert>

#include <spdlog/spdlog.h>

#include "harness/test_util.h"

namespace raftpp {

Network::Network() : rng_(std::random_device{}()) {}

Config Network::DefaultConfig() {
    Config config = raftpp::DefaultConfig();
    config.election_tick = 10;
    config.heartbeat_tick = 1;
    config.max_size_per_message = NO_LIMIT;
    config.max_inflight_messages = 256;
    return config;
}

Network Network::Create(size_t num_peers) {
    std::vector<std::unique_ptr<Interface>> peers(num_peers);
    return Create(std::move(peers));
}

Network Network::Create(std::vector<std::unique_ptr<Interface>> peers) {
    Config config = DefaultConfig();
    return CreateWithConfig(std::move(peers), config);
}

Network Network::CreateWithConfig(
    std::vector<std::unique_ptr<Interface>> peers, const Config& config
) {
    Network network;

    std::vector<uint64_t> peer_ids;
    for (size_t i = 0; i < peers.size(); ++i) {
        peer_ids.push_back(i + 1);
    }

    for (size_t i = 0; i < peers.size(); ++i) {
        uint64_t id = i + 1;

        if (peers[i] == nullptr) {
            // Create new raft with default config
            auto storage = std::make_shared<MemoryStorage>();

            // Initialize with conf state directly (not via ApplySnapshot
            // because first_index() > snap.index() would cause SnapshotOutOfDate)
            ConfState conf_state;
            auto conf_builder = conf_state.builder();
            auto voters = conf_builder.initVoters(peer_ids.size());
            for (size_t j = 0; j < peer_ids.size(); ++j) {
                voters.set(j, peer_ids[j]);
            }
            storage->SetConfState(conf_state);

            Config node_config = config;
            node_config.id = id;

            // Use NewTestRaftWithConfig which handles storage correctly
            auto interface = NewTestRaftWithConfig(node_config, storage);

            network.storage_[id] = storage;
            network.peers_.emplace(id, std::move(interface));
        } else {
            // Use provided peer
            if (peers[i]->HasRaft()) {
                assert((*peers[i])->id() == id && "peer has wrong position");
                network.storage_[id] = peers[i]->GetStorage();
            }
            network.peers_.emplace(id, std::move(*peers[i]));
        }
    }

    return network;
}

void Network::IgnoreMessageType(MessageType type) {
    ignore_map_[type] = true;
}

std::vector<Message> Network::Filter(std::vector<Message> msgs) {
    std::vector<Message> result;
    result.reserve(msgs.size());

    std::uniform_real_distribution<double> dist(0.0, 1.0);

    for (auto& m : msgs) {
        auto reader = m.reader();
        // Check if message type is ignored
        auto it = ignore_map_.find(reader.getMsgType());
        if (it != ignore_map_.end() && it->second) {
            continue;
        }

        // MsgHup should never go over network
        assert(reader.getMsgType() != MessageType::MSG_HUP && "unexpected MsgHup");

        // Check drop rate
        Connection conn{reader.getFrom(), reader.getTo()};
        auto drop_it = drop_map_.find(conn);
        double perc = (drop_it != drop_map_.end()) ? drop_it->second : 0.0;

        if (dist(rng_) >= perc) {
            result.push_back(std::move(m));
        }
    }

    return result;
}

std::vector<Message> Network::ReadMessages() {
    std::vector<Message> all_msgs;
    for (auto& [id, peer] : peers_) {
        auto msgs = peer.ReadMessages();
        all_msgs.insert(
            all_msgs.end(), std::make_move_iterator(msgs.begin()),
            std::make_move_iterator(msgs.end())
        );
    }
    return all_msgs;
}

void Network::Send(std::vector<Message> msgs) {
    while (!msgs.empty()) {
        std::vector<Message> new_msgs;

        for (auto& m : msgs) {
            auto reader = m.reader();
            SPDLOG_DEBUG(
                "Network::Send: type={}, from={}, to={}", static_cast<int>(reader.getMsgType()), reader.getFrom(),
                reader.getTo()
            );
            auto it = peers_.find(reader.getTo());
            if (it == peers_.end()) {
                continue;
            }

            auto& peer = it->second;
            auto result = peer.Step(m);
            // Ignore errors from Step, just like in the Rust code
            (void)result;
            peer.Persist();

            auto resp = peer.ReadMessages();
            auto filtered = Filter(std::move(resp));
            new_msgs.insert(
                new_msgs.end(), std::make_move_iterator(filtered.begin()),
                std::make_move_iterator(filtered.end())
            );
        }

        msgs = std::move(new_msgs);
    }
}

void Network::FilterAndSend(std::vector<Message> msgs) {
    Send(Filter(std::move(msgs)));
}

Result<void> Network::Dispatch(std::vector<Message> msgs) {
    auto filtered = Filter(std::move(msgs));
    for (auto& m : filtered) {
        auto it = peers_.find(m.reader().getTo());
        if (it == peers_.end()) {
            continue;
        }
        auto result = it->second.Step(m);
        if (!result) {
            return result;
        }
    }
    return {};
}

void Network::Drop(uint64_t from, uint64_t to, double perc) {
    drop_map_[Connection{from, to}] = perc;
}

void Network::Cut(uint64_t one, uint64_t other) {
    Drop(one, other, 1.0);
    Drop(other, one, 1.0);
}

void Network::Isolate(uint64_t id) {
    for (size_t i = 0; i < peers_.size(); ++i) {
        uint64_t nid = i + 1;
        if (nid != id) {
            Drop(id, nid, 1.0);
            Drop(nid, id, 1.0);
        }
    }
}

void Network::Recover() {
    drop_map_.clear();
    ignore_map_.clear();
}

Interface* Network::GetPeer(uint64_t id) {
    auto it = peers_.find(id);
    return it != peers_.end() ? &it->second : nullptr;
}

const Interface* Network::GetPeer(uint64_t id) const {
    auto it = peers_.find(id);
    return it != peers_.end() ? &it->second : nullptr;
}

std::shared_ptr<MemoryStorage> Network::GetStorage(uint64_t id) {
    auto it = storage_.find(id);
    return it != storage_.end() ? it->second : nullptr;
}

Network CreateTestNetwork(size_t num_peers) {
    return Network::Create(num_peers);
}

Network CreateTestNetworkWithConfig(size_t num_peers, const Config& config) {
    std::vector<std::unique_ptr<Interface>> peers(num_peers);
    return Network::CreateWithConfig(std::move(peers), config);
}

}  // namespace raftpp
