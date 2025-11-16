#pragma once

#include <optional>
#include <vector>

#include "raftpp/raft.h"
#include "raftpp/raftpp.pb.h"

namespace raftpp {

struct Peer {
    /// The ID of the peer.
    uint64_t id;
    /// If there is context associated with the peer (like connection information), it can be
    /// serialized and stored here.
    std::optional<std::vector<uint8_t>> context;
};

enum class SnapshotStatus : uint8_t {
    /// Represents that the snapshot is finished being created.
    Finish,
    /// Indicates that the snapshot failed to build or is not ready.
    Failure,
};

bool IsLocalMessage(MessageType t);
bool IsResponseMessage(MessageType t);

struct LightReady {
    std::optional<uint64_t> commit_index;
    std::vector<Entry> committed_entries;
    std::vector<Message> messages;
};

struct Ready {
    uint64_t number;
    std::optional<SoftState> ss;
    std::optional<HardState> hs;
    std::vector<ReadState> read_states;
    std::vector<Entry> entries;
    Snapshot snapshot;
    bool is_persisted_msg;
    LightReady light;
    bool must_sync;
};

struct ReadyRecord {
    uint64_t number;
    // (index, term) of the last entry from the entries in Ready
    std::optional<std::pair<uint64_t, uint64_t>> last_entry;
    // (index, term) of the snapshot in Ready
    std::optional<std::pair<uint64_t, uint64_t>> snapshot;
};

class RawNode {
public:
    RawNode(const Config &config, std::unique_ptr<Storage> store);

private:
    Raft raft_;
    SoftState prev_ss_;
    HardState prev_hs_;
    uint64_t max_number_;
    std::deque<ReadyRecord> records_;
    uint64_t commit_since_index_;
};

}
