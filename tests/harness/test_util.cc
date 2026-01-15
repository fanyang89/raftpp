#include "harness/test_util.h"

#include <spdlog/fmt/fmt.h>

namespace raftpp {

constexpr const char* SOME_DATA = "somedata";

Config NewTestConfig(uint64_t id, size_t election_tick, size_t heartbeat_tick) {
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
        // Initialize with conf state using ApplySnapshot (matches raft-rs behavior)
        Snapshot snap;
        snap.mutable_metadata()->set_index(0);
        snap.mutable_metadata()->set_term(0);
        for (uint64_t peer_id : peers) {
            snap.mutable_metadata()->mutable_conf_state()->add_voters(peer_id);
        }
        storage->ApplySnapshot(snap).value();
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
        Snapshot snap;
        snap.mutable_metadata()->set_index(0);
        snap.mutable_metadata()->set_term(0);
        for (uint64_t peer_id : peers) {
            snap.mutable_metadata()->mutable_conf_state()->add_voters(peer_id);
        }
        storage->ApplySnapshot(snap).value();
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
        Snapshot snap;
        snap.mutable_metadata()->set_index(0);
        snap.mutable_metadata()->set_term(0);
        for (uint64_t peer_id : peers) {
            snap.mutable_metadata()->mutable_conf_state()->add_voters(peer_id);
        }
        storage->ApplySnapshot(snap).value();
    }

    storage->Append(logs).value();
    return NewTestRaftWithConfig(config, storage);
}

Interface NewTestRaftWithConfig(const Config& config, std::shared_ptr<MemoryStorage> storage) {
    // For tests that use ApplySnapshot on the storage before calling this function,
    // we need to ensure the conf_state is preserved
    auto initial_state = storage->InitialState();
    if (initial_state) {
        // Storage already has state, ensure conf_state is properly set
        // by calling SetRaftState with the current conf_state
        auto conf_state = initial_state->conf_state;
        auto hard_state = initial_state->hard_state;
        storage->SetRaftState({hard_state, conf_state});
    }

    // Create a new owned storage but preserve the shared storage's state
    auto owned_storage = std::make_unique<MemoryStorage>();
    if (initial_state) {
        owned_storage->SetRaftState({initial_state->hard_state, initial_state->conf_state});
    }

    // Copy entries
    auto entries = storage->AllEntries();
    if (!entries.empty()) {
        owned_storage->Append(entries).value();
    }

    auto raft = std::make_unique<Raft>(config, std::move(owned_storage));
    return Interface(std::move(raft), storage);
}

HardState MakeHardState(uint64_t term, uint64_t commit, uint64_t vote) {
    HardState hs;
    hs.set_term(term);
    hs.set_commit(commit);
    hs.set_vote(vote);
    return hs;
}

SoftState MakeSoftState(uint64_t leader_id, StateRole state) {
    return SoftState{leader_id, state};
}

Message NewMessageWithEntries(
    uint64_t from, uint64_t to, MessageType type, std::vector<Entry> entries
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

Message NewMessage(uint64_t from, uint64_t to, MessageType type, size_t n) {
    std::vector<Entry> entries;
    entries.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        entries.push_back(NewEntry(0, 0, SOME_DATA));
    }
    return NewMessageWithEntries(from, to, type, std::move(entries));
}

Entry NewEntry(uint64_t term, uint64_t index, const std::optional<std::string>& data) {
    Entry e;
    e.set_term(term);
    e.set_index(index);
    if (data) {
        e.set_data(*data);
    }
    return e;
}

Entry EmptyEntry(uint64_t term, uint64_t index) {
    return NewEntry(term, index, std::nullopt);
}

Snapshot NewSnapshot(uint64_t index, uint64_t term, const std::vector<uint64_t>& voters) {
    Snapshot snap;
    snap.mutable_metadata()->set_index(index);
    snap.mutable_metadata()->set_term(term);
    for (uint64_t voter : voters) {
        snap.mutable_metadata()->mutable_conf_state()->add_voters(voter);
    }
    return snap;
}

ConfChange MakeConfChange(ConfChangeType type, uint64_t node_id) {
    ConfChange cc;
    cc.set_change_type(type);
    cc.set_node_id(node_id);
    return cc;
}

ConfChangeV2 MakeRemoveNodeCC(uint64_t node_id) {
    ConfChangeV2 cc;
    auto* change = cc.add_changes();
    change->set_change_type(ConfChangeType::RemoveNode);
    change->set_node_id(node_id);
    return cc;
}

ConfChangeV2 MakeAddNodeCC(uint64_t node_id) {
    ConfChangeV2 cc;
    auto* change = cc.add_changes();
    change->set_change_type(ConfChangeType::AddNode);
    change->set_node_id(node_id);
    return cc;
}

ConfChangeV2 MakeAddLearnerCC(uint64_t node_id) {
    ConfChangeV2 cc;
    auto* change = cc.add_changes();
    change->set_change_type(ConfChangeType::AddLearnerNode);
    change->set_node_id(node_id);
    return cc;
}

ConfChangeV2 MakeConfChangeV2Single(ConfChangeType type, uint64_t node_id) {
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
    const std::vector<uint64_t>& terms, bool pre_vote, uint64_t id,
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
    uint64_t vote, uint64_t term, bool pre_vote, uint64_t id, const std::vector<uint64_t>& peers
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
        const auto& last_entry = unstable_entries.back();
        const uint64_t last_idx = last_entry.index();
        const uint64_t last_term = last_entry.term();
        raft_log.StableEntries(last_idx, last_term);
        s.Append(unstable_entries).value();
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

}  // namespace raftpp
