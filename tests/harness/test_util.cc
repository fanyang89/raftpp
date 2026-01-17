#include "harness/test_util.h"

#include <doctest/doctest.h>
#include <google/protobuf/util/message_differencer.h>
#include <spdlog/fmt/fmt.h>

namespace raftpp {

constexpr const char* SOME_DATA = "somedata";

Config NewTestConfig(const uint64_t id, const size_t election_tick, const size_t heartbeat_tick) {
    Config config = DefaultConfig();
    config.id = id;
    config.election_tick = election_tick;
    config.heartbeat_tick = heartbeat_tick;
    config.max_size_per_message = NO_LIMIT;
    config.max_inflight_messages = 256;
    config.load_state_on_startup = true;  // Enable loading initial state for tests
    return config;
}

Interface NewTestRaft(
    uint64_t id, const std::vector<uint64_t>& peers, size_t election, size_t heartbeat,
    std::shared_ptr<MemoryStorage> storage
) {
    Config config = NewTestConfig(id, election, heartbeat);

    // Initialize storage with peers if needed
    auto initial_state = storage->InitialState();
    if (initial_state && !initial_state->conf_state.voters().empty()) {
        // Already initialized
        if (peers.empty()) {
            throw std::runtime_error("NewTestRaft with empty peers on initialized store");
        }
    } else if (!peers.empty()) {
        // Initialize with conf state directly using SetRaftState
        // This ensures conf_state is properly set in storage
        ConfState conf_state;
        for (uint64_t peer_id : peers) {
            conf_state.add_voters(peer_id);
        }
        // Ensure hard state has commit=0 to match raft-rs behavior
        HardState hard_state;
        hard_state.set_commit(0);
        hard_state.set_term(0);
        hard_state.set_vote(0);
        storage->SetRaftState({hard_state, conf_state});
    }

    return NewTestRaftWithConfig(config, storage);
}

Interface NewTestRaftWithPrevote(
    uint64_t id, const std::vector<uint64_t>& peers, size_t election, size_t heartbeat,
    std::shared_ptr<MemoryStorage> storage, bool pre_vote
) {
    Config config = NewTestConfig(id, election, heartbeat);
    config.pre_vote = pre_vote;

    auto initial_state = storage->InitialState();
    if (initial_state && !initial_state->conf_state.voters().empty()) {
        if (peers.empty()) {
            throw std::runtime_error("NewTestRaft with empty peers on initialized store");
        }
    } else if (!peers.empty()) {
        // Initialize with conf state directly using SetRaftState
        ConfState conf_state;
        for (uint64_t peer_id : peers) {
            conf_state.add_voters(peer_id);
        }
        HardState hard_state;
        hard_state.set_commit(0);
        hard_state.set_term(0);
        hard_state.set_vote(0);
        storage->SetRaftState({hard_state, conf_state});
    }

    return NewTestRaftWithConfig(config, storage);
}

Interface NewTestRaftWithLogs(
    uint64_t id, const std::vector<uint64_t>& peers, size_t election, size_t heartbeat,
    std::shared_ptr<MemoryStorage> storage, const std::vector<Entry>& logs
) {
    Config config = NewTestConfig(id, election, heartbeat);

    auto initial_state = storage->InitialState();
    if (initial_state && !initial_state->conf_state.voters().empty()) {
        if (peers.empty()) {
            throw std::runtime_error("NewTestRaft with empty peers on initialized store");
        }
    } else if (!peers.empty()) {
        // Use SetRaftState instead of ApplySnapshot
        ConfState conf_state;
        for (uint64_t peer_id : peers) {
            conf_state.add_voters(peer_id);
        }
        HardState hard_state;
        hard_state.set_commit(0);
        hard_state.set_term(0);
        hard_state.set_vote(0);
        storage->SetRaftState({hard_state, conf_state});
    }

    storage->Append(logs).value();
    return NewTestRaftWithConfig(config, storage);
}

Interface NewTestRaftWithConfig(const Config& config, std::shared_ptr<MemoryStorage> storage) {
    // Create a new owned storage and copy state from the shared storage
    auto owned_storage = std::make_unique<MemoryStorage>();

    // Get the initial state from shared storage (which may have ApplySnapshot applied)
    auto initial_state = storage->InitialState();

    // Check if storage has a snapshot by comparing first_index and last_index
    auto first_idx = storage->FirstIndex().value();
    uint64_t snapshot_index = first_idx - 1;  // first_index = snapshot.index + 1

    // If there's a snapshot, we need to apply it to owned_storage first
    if (snapshot_index > 0) {
        Snapshot snap;
        snap.mutable_metadata()->set_index(snapshot_index);
        // Get term for snapshot index from storage
        auto term = storage->Term(snapshot_index);
        if (term) {
            snap.mutable_metadata()->set_term(term.value());
        }
        // Copy conf state
        if (initial_state) {
            snap.mutable_metadata()->mutable_conf_state()->CopyFrom(initial_state->conf_state);
        }
        owned_storage->ApplySnapshot(snap).value();
    }

    // Set raft state - ensure commit is at least snapshot_index
    if (initial_state) {
        HardState hard_state = initial_state->hard_state;
        // Commit should never be less than snapshot index
        if (hard_state.commit() < snapshot_index) {
            hard_state.set_commit(snapshot_index);
        }
        owned_storage->SetRaftState({hard_state, initial_state->conf_state});
    }

    // Copy entries from shared storage
    auto entries = storage->AllEntries();
    if (!entries.empty()) {
        owned_storage->Append(entries).value();
    }

    auto raft = std::make_unique<Raft>(config, std::move(owned_storage));
    return Interface(std::move(raft), storage);
}

HardState MakeHardState(const uint64_t term, const uint64_t commit, const uint64_t vote) {
    HardState hs;
    hs.set_term(term);
    hs.set_commit(commit);
    hs.set_vote(vote);
    return hs;
}

SoftState MakeSoftState(const uint64_t leader_id, const StateRole state) {
    return SoftState{leader_id, state};
}

Message NewMessageWithEntries(
    const uint64_t from, const uint64_t to, const MessageType type, std::vector<Entry> entries
) {
    Message m;
    m.set_msg_type(type);
    m.set_to(to);
    m.set_from(from);
    for (auto& e : entries) {
        *m.add_entries() = std::move(e);
    }
    return m;
}

Message NewMessage(const uint64_t from, const uint64_t to, const MessageType type, const size_t n) {
    std::vector<Entry> entries;
    entries.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        entries.push_back(NewEntry(0, 0, SOME_DATA));
    }
    return NewMessageWithEntries(from, to, type, std::move(entries));
}

Entry NewEntry(const uint64_t index, const uint64_t term, const std::optional<std::string>& data) {
    Entry e;
    e.set_index(index);
    e.set_term(term);
    if (data.has_value()) {
        e.set_data(*data);
    }
    return e;
}

Entry EmptyEntry(const uint64_t index, const uint64_t term) {
    return NewEntry(index, term, std::nullopt);
}

Snapshot NewSnapshot(
    const uint64_t index, const uint64_t term, const std::vector<uint64_t>& voters
) {
    Snapshot snap;
    snap.mutable_metadata()->set_index(index);
    snap.mutable_metadata()->set_term(term);
    snap.mutable_metadata()->mutable_conf_state()->mutable_voters()->Add(
        voters.begin(), voters.end()
    );
    return snap;
}

ConfChange MakeConfChange(const ConfChangeType type, const uint64_t node_id) {
    ConfChange cc;
    cc.set_change_type(type);
    cc.set_node_id(node_id);
    return cc;
}

ConfChangeV2 MakeRemoveNodeCC(const uint64_t node_id) {
    ConfChangeV2 cc;
    auto* change = cc.add_changes();
    change->set_change_type(ConfChangeType::RemoveNode);
    change->set_node_id(node_id);
    return cc;
}

ConfChangeV2 MakeAddNodeCC(const uint64_t node_id) {
    ConfChangeV2 cc;
    auto* change = cc.add_changes();
    change->set_change_type(ConfChangeType::AddNode);
    change->set_node_id(node_id);
    return cc;
}

ConfChangeV2 MakeAddLearnerCC(const uint64_t node_id) {
    ConfChangeV2 cc;
    auto* change = cc.add_changes();
    change->set_change_type(ConfChangeType::AddLearnerNode);
    change->set_node_id(node_id);
    return cc;
}

ConfChangeV2 MakeConfChangeV2Single(const ConfChangeType type, const uint64_t node_id) {
    ConfChangeV2 cc;
    auto* change = cc.add_changes();
    change->set_change_type(type);
    change->set_node_id(node_id);
    return cc;
}

ConfState MakeConfState(
    const std::vector<uint64_t>& voters, const std::vector<uint64_t>& learners
) {
    ConfState cs;
    for (uint64_t voter : voters) {
        cs.add_voters(voter);
    }
    for (uint64_t learner : learners) {
        cs.add_learners(learner);
    }
    return cs;
}

std::string LogToString(const RaftLog& raft_log) {
    std::string s = fmt::format("committed: {}\n", raft_log.committed());
    s += fmt::format("applied: {}\n", raft_log.applied());

    auto entries = const_cast<RaftLog&>(raft_log).AllEntries();
    for (size_t i = 0; i < entries.size(); ++i) {
        s += fmt::format("#{}: index={} term={}\n", i, entries[i].index(), entries[i].term());
    }
    return s;
}

std::unique_ptr<Interface> NopStepper() {
    return std::make_unique<Interface>();
}

Interface EntsWithConfig(
    const std::vector<uint64_t>& terms, const bool pre_vote, const uint64_t id,
    const std::vector<uint64_t>& peers
) {
    auto storage = std::make_shared<MemoryStorage>();

    // Initialize with conf state
    Snapshot snap;
    snap.mutable_metadata()->set_index(0);
    snap.mutable_metadata()->set_term(0);
    for (uint64_t peer_id : peers) {
        snap.mutable_metadata()->mutable_conf_state()->add_voters(peer_id);
    }
    storage->ApplySnapshot(snap).value();

    // Append entries
    std::vector<Entry> entries;
    for (size_t i = 0; i < terms.size(); ++i) {
        Entry e;
        e.set_index(i + 1);
        e.set_term(terms[i]);
        entries.push_back(std::move(e));
    }
    storage->Append(entries).value();

    auto raft = NewTestRaftWithPrevote(id, {}, 5, 1, storage, pre_vote);
    // Reset to last term
    // Note: In Rust this calls raft.reset(terms.last()), but we don't expose reset
    // The raft should already be in the correct state after construction
    return raft;
}

Interface VotedWithConfig(
    const uint64_t vote, const uint64_t term, const bool pre_vote, const uint64_t id,
    const std::vector<uint64_t>& peers
) {
    auto storage = std::make_shared<MemoryStorage>();

    // Initialize with conf state
    Snapshot snap;
    snap.mutable_metadata()->set_index(0);
    snap.mutable_metadata()->set_term(0);
    for (uint64_t peer_id : peers) {
        snap.mutable_metadata()->mutable_conf_state()->add_voters(peer_id);
    }
    storage->ApplySnapshot(snap).value();

    // Set hard state with vote and term
    HardState hs;
    hs.set_vote(vote);
    hs.set_term(term);
    storage->SetRaftState({hs, {}});

    return NewTestRaftWithPrevote(id, {}, 5, 1, storage, pre_vote);
}

std::vector<Entry> NextEntries(Raft& r, MemoryStorage& s) {
    auto& raft_log = r.raft_log();

    // Persist unstable entries
    const auto& unstable_entries = raft_log.unstable().entries();
    if (!unstable_entries.empty()) {
        // Make a copy BEFORE calling StableEntries, which clears them
        std::vector<Entry> entries_to_persist(unstable_entries.begin(), unstable_entries.end());
        const auto& last_entry = entries_to_persist.back();
        const uint64_t last_idx = last_entry.index();
        const uint64_t last_term = last_entry.term();

        // First append to storage, then mark as stable
        s.Append(entries_to_persist).value();

        // Also update the internal storage
        auto* internal_storage = dynamic_cast<MemoryStorage*>(raft_log.storage());
        if (internal_storage) {
            internal_storage->Append(entries_to_persist).value();
        }

        // Now mark as stable
        raft_log.StableEntries(last_idx, last_term);
        r.OnPersistEntries(last_idx, last_term);
    }

    // Get next entries
    auto ents = raft_log.NextEntries(std::nullopt);
    r.CommitApply(raft_log.committed());
    return ents.value_or(std::vector<Entry>{});
}

void CommitNoopEntry(Network& network, MemoryStorage& storage, Raft& raft) {
    // This helper commits the initial no-op entry after leader election
    // by having the leader broadcast and receive responses

    // First append a no-op entry to make LastIndex > 0
    // This ensures BroadcastAppend will send entries
    Entry noop_entry;
    noop_entry.set_term(raft.term());
    noop_entry.set_index(raft.raft_log().LastIndex() + 1);
    raft.AppendEntry(noop_entry);

    // Now broadcast append messages (which will include the no-op entry)
    raft.BroadcastAppend();

    auto msgs = raft.messages();
    raft.messages().clear();

    for (auto& msg : msgs) {
        if (msg.msg_type() == MsgAppend) {
            // Create response
            Message resp;
            resp.set_msg_type(MsgAppendResponse);
            resp.set_from(msg.to());
            resp.set_to(msg.from());
            resp.set_term(raft.term());
            resp.set_index(msg.index() + msg.entries_size());

            raft.Step(resp);
        }
    }

    // Persist and commit
    NextEntries(raft, storage);
    raft.messages().clear();
}

Snapshot TestingSnapshot() {
    return NewSnapshot(11, 11, {1, 2});
}

// ============================================================================
// Raw Node Test Helpers
// ============================================================================

RawNode NewRawNode(
    uint64_t id, const std::vector<uint64_t>& peers, size_t election_tick, size_t heartbeat_tick,
    std::shared_ptr<MemoryStorage> storage
) {
    Config config = DefaultConfig();
    config.id = id;
    config.election_tick = election_tick;
    config.heartbeat_tick = heartbeat_tick;
    config.max_size_per_message = NO_LIMIT;
    config.max_inflight_messages = 256;
    config.load_state_on_startup = true;

    auto initial_state = storage->InitialState();
    bool is_initialized = initial_state && !initial_state->conf_state.voters().empty();

    // If storage is already initialized, just use it as-is (empty peers means use existing config)
    // If storage is NOT initialized and peers is provided, initialize with snapshot
    if (!is_initialized && !peers.empty()) {
        storage->ApplySnapshot(NewSnapshot(1, 1, peers)).value();
    }

    return RawNode(config, std::move(storage));
}

RawNode NewRawNodeWithConfig(
    const std::vector<uint64_t>& peers, const Config& config, std::shared_ptr<MemoryStorage> storage
) {
    auto initial_state = storage->InitialState();
    bool is_initialized = initial_state && !initial_state->conf_state.voters().empty();

    // If storage is already initialized, just use it as-is (empty peers means use existing config)
    // If storage is NOT initialized and peers is provided, initialize with snapshot
    if (!is_initialized && !peers.empty()) {
        storage->ApplySnapshot(NewSnapshot(1, 1, peers)).value();
    }

    return RawNode(config, std::move(storage));
}

bool operator==(const std::optional<HardState>& e1, const std::optional<HardState>& e2) {
    if (e1.has_value() != e2.has_value()) {
        return false;
    }
    if (!e1.has_value() && !e2.has_value()) {
        return true;
    }
    // Use MessageDifferencer directly to avoid recursive call
    return google::protobuf::util::MessageDifferencer::Equals(*e1, *e2);
}

bool operator==(const Snapshot& e1, const Snapshot& e2) {
    return google::protobuf::util::MessageDifferencer::Equals(e1, e2);
}

bool operator==(const Entry& e1, const Entry& e2) {
    return google::protobuf::util::MessageDifferencer::Equals(e1, e2);
}

bool operator==(const std::vector<Entry>& e1, const std::vector<Entry>& e2) {
    if (e1.size() != e2.size()) {
        return false;
    }

    for (size_t i = 0; i < e1.size(); ++i) {
        if (e1.at(i) != e2.at(i)) {
            return false;
        }
    }
    return true;
}

bool operator==(Result<Snapshot> e1, Result<Snapshot> e2) {
    if (e1.has_value() != e2.has_value()) {
        return false;
    }
    if (e1.has_value()) {
        return *e1 == *e2;
    }
    return e1.error() == e2.error();
}

bool operator==(const HardState& e1, const HardState& e2) {
    return google::protobuf::util::MessageDifferencer::Equals(e1, e2);
}

bool operator==(const ReadState& e1, const ReadState& e2) {
    return e1.index == e2.index && e1.request_ctx == e2.request_ctx;
}

bool operator==(const std::vector<ReadState>& e1, const std::vector<ReadState>& e2) {
    if (e1.size() != e2.size()) {
        return false;
    }
    for (size_t i = 0; i < e1.size(); ++i) {
        if (!(e1[i] == e2[i])) {
            return false;
        }
    }
    return true;
}

bool operator==(const ConfState& e1, const ConfState& e2) {
    // Use MessageDifferencer with TreatAsSet for repeated fields
    // because the order of voters/learners may differ
    google::protobuf::util::MessageDifferencer diff;
    diff.TreatAsSet(ConfState::descriptor()->FindFieldByName("voters"));
    diff.TreatAsSet(ConfState::descriptor()->FindFieldByName("learners"));
    diff.TreatAsSet(ConfState::descriptor()->FindFieldByName("voters_outgoing"));
    diff.TreatAsSet(ConfState::descriptor()->FindFieldByName("learners_next"));
    return diff.Compare(e1, e2);
}

void MustCmpReady(
    const Ready& rd, const std::optional<SoftState>& ss, const std::optional<HardState>& hs,
    const std::vector<Entry>& entries, const std::vector<Entry>& committed_entries,
    const std::optional<Snapshot>& snapshot, bool msg_is_empty, bool persisted_msg_is_empty,
    bool must_sync
) {
    CHECK_EQ(rd.ss, ss);
    CHECK_EQ(rd.hs, hs);
    CHECK_EQ(rd.entries, entries);
    CHECK_EQ(rd.light.committed_entries, committed_entries);
    CHECK_EQ(rd.read_states.empty(), true);

    Snapshot default_snap;
    CHECK_EQ(rd.snapshot, snapshot.value_or(default_snap));

    CHECK_EQ(rd.Messages().empty(), msg_is_empty);
    CHECK_EQ(rd.light.messages.empty(), persisted_msg_is_empty);
    CHECK_EQ(rd.must_sync, must_sync);
}

ConfState MakeConfStateV2(
    const std::vector<uint64_t>& voters, const std::vector<uint64_t>& learners,
    const std::vector<uint64_t>& voters_outgoing, const std::vector<uint64_t>& learners_next,
    const bool auto_leave
) {
    ConfState cs;
    for (uint64_t voter : voters) {
        cs.add_voters(voter);
    }
    for (uint64_t learner : learners) {
        cs.add_learners(learner);
    }
    for (uint64_t voter : voters_outgoing) {
        cs.add_voters_outgoing(voter);
    }
    for (uint64_t learner : learners_next) {
        cs.add_learners_next(learner);
    }
    cs.set_auto_leave(auto_leave);
    return cs;
}

}  // namespace raftpp
