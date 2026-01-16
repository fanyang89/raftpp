#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "harness/interface.h"
#include "harness/network.h"
#include "raftpp/memory_storage.h"
#include "raftpp/raft.h"
#include "raftpp/raft_config.h"
#include "raftpp/raft_core.h"
#include "raftpp/raft_log.h"
#include "raftpp/raftpp.pb.h"
#include "raftpp/raw_node.h"

namespace raftpp {

constexpr uint64_t NO_LIMIT = std::numeric_limits<uint64_t>::max();

/// Create a test config with given parameters.
Config NewTestConfig(uint64_t id, size_t election_tick, size_t heartbeat_tick);

/// Create a new test raft instance.
Interface NewTestRaft(
    uint64_t id, const std::vector<uint64_t>& peers, size_t election, size_t heartbeat,
    std::shared_ptr<MemoryStorage> storage
);

/// Create a new test raft instance with prevote option.
Interface NewTestRaftWithPrevote(
    uint64_t id, const std::vector<uint64_t>& peers, size_t election, size_t heartbeat,
    std::shared_ptr<MemoryStorage> storage, bool pre_vote
);

/// Create a new test raft with logs.
Interface NewTestRaftWithLogs(
    uint64_t id, const std::vector<uint64_t>& peers, size_t election, size_t heartbeat,
    std::shared_ptr<MemoryStorage> storage, const std::vector<Entry>& logs
);

/// Create a new test raft with config.
Interface NewTestRaftWithConfig(const Config& config, std::shared_ptr<MemoryStorage> storage);

/// Create a HardState.
HardState MakeHardState(uint64_t term, uint64_t commit, uint64_t vote);

/// Create a SoftState.
SoftState MakeSoftState(uint64_t leader_id, StateRole state);

/// Create a message with entries.
Message NewMessageWithEntries(
    uint64_t from, uint64_t to, MessageType type, std::vector<Entry> entries
);

/// Create a message with n entries containing "somedata".
Message NewMessage(uint64_t from, uint64_t to, MessageType type, size_t n = 0);

/// Create an entry.
Entry NewEntry(
    uint64_t term, uint64_t index, const std::optional<std::string>& data = std::nullopt
);

/// Create an empty entry.
Entry EmptyEntry(uint64_t term, uint64_t index);

/// Create a snapshot.
Snapshot NewSnapshot(uint64_t index, uint64_t term, const std::vector<uint64_t>& voters);

/// Create a ConfChange.
ConfChange MakeConfChange(ConfChangeType type, uint64_t node_id);

/// Create a ConfChangeV2 to remove a node.
ConfChangeV2 MakeRemoveNodeCC(uint64_t node_id);

/// Create a ConfChangeV2 to add a node.
ConfChangeV2 MakeAddNodeCC(uint64_t node_id);

/// Create a ConfChangeV2 to add a learner.
ConfChangeV2 MakeAddLearnerCC(uint64_t node_id);

/// Create a ConfChangeV2 with a single change.
ConfChangeV2 MakeConfChangeV2Single(ConfChangeType type, uint64_t node_id);

/// Create a ConfState.
ConfState MakeConfState(
    const std::vector<uint64_t>& voters, const std::vector<uint64_t>& learners = {}
);

/// Convert a RaftLog to string for debugging.
std::string LogToString(const RaftLog& raft_log);

/// Create a no-op interface (placeholder node).
std::unique_ptr<Interface> NopStepper();

/// Create entries with given terms (index starts at 1).
Interface EntsWithConfig(
    const std::vector<uint64_t>& terms, bool pre_vote, uint64_t id,
    const std::vector<uint64_t>& peers
);

/// Create a raft state machine with vote and term set but no log entries.
Interface VotedWithConfig(
    uint64_t vote, uint64_t term, bool pre_vote, uint64_t id, const std::vector<uint64_t>& peers
);

/// Persist committed index and fetch next entries.
std::vector<Entry> NextEntries(Raft& r, MemoryStorage& s);

/// Helper to commit a no-op entry (used in paper tests).
void CommitNoopEntry(Network& network, MemoryStorage& storage, Raft& raft);

/// Create a testing snapshot.
Snapshot TestingSnapshot();

// ============================================================================
// Raw Node Test Helpers
// ============================================================================

/// Create a RawNode for testing.
RawNode NewRawNode(
    uint64_t id, const std::vector<uint64_t>& peers, size_t election_tick, size_t heartbeat_tick,
    std::shared_ptr<MemoryStorage> storage
);

/// Create a RawNode with custom config.
RawNode NewRawNodeWithConfig(
    const std::vector<uint64_t>& peers, const Config& config, std::shared_ptr<MemoryStorage> storage
);

/// Compare Ready with expected values.
void MustCmpReady(
    const Ready& rd, const std::optional<SoftState>& ss, const std::optional<HardState>& hs,
    const std::vector<Entry>& entries, const std::vector<Entry>& committed_entries,
    const std::optional<Snapshot>& snapshot, bool msg_is_empty, bool persisted_msg_is_empty,
    bool must_sync
);

/// Create ConfState for joint config.
ConfState MakeConfStateV2(
    const std::vector<uint64_t>& voters, const std::vector<uint64_t>& learners,
    const std::vector<uint64_t>& voters_outgoing, const std::vector<uint64_t>& learners_next,
    bool auto_leave
);

bool operator==(const std::optional<HardState>& e1, const std::optional<HardState>& e2);
bool operator==(const Snapshot& e1, const Snapshot& e2);
bool operator==(const Entry& e1, const Entry& e2);
bool operator==(const std::vector<Entry>& e1, const std::vector<Entry>& e2);
bool operator==(Result<Snapshot> e1, Result<Snapshot> e2);

}  // namespace raftpp
