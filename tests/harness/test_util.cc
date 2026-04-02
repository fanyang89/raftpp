#include "harness/test_util.h"

#include <set>
#include <stdexcept>
#include <tuple>

#include <capnp/blob.h>
#include <capnp/common.h>
#include <doctest/doctest.h>
#include <kj/common.h>

#include "raftpp.capnp.h"
#include "raftpp/core/capnp_util.h"
#include "raftpp/core/memory_storage.h"
#include "raftpp/core/raft.h"
#include "raftpp/core/raft_log.h"
#include "raftpp/core/read_only.h"
#include "raftpp/core/unstable_log.h"
#include "raftpp/fmt.h"

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
    if (initial_state) {
        auto conf_reader = capnp_util::reader<msg::ConfState>(initial_state->conf_state);
        if (conf_reader.getVoters().size() > 0) {
            // Already initialized
            if (peers.empty()) {
                throw std::runtime_error("NewTestRaft with empty peers on initialized store");
            }
        } else if (!peers.empty()) {
            // Initialize with conf state
            ConfState conf_state = capnp_util::make<msg::ConfState>();
            auto conf_builder = capnp_util::builder<msg::ConfState>(conf_state);
            auto voters = conf_builder.initVoters(peers.size());
            for (size_t i = 0; i < peers.size(); ++i) {
                voters.set(i, peers[i]);
            }

            HardState hard_state = capnp_util::make<msg::HardState>();
            auto hs_builder = capnp_util::builder<msg::HardState>(hard_state);
            hs_builder.setCommit(0);
            hs_builder.setTerm(0);
            hs_builder.setVote(0);

            RaftState raft_state;
            raft_state.hard_state = std::move(hard_state);
            raft_state.conf_state = std::move(conf_state);
            storage->SetRaftState(std::move(raft_state));
        }
    } else if (!peers.empty()) {
        // Initialize with conf state
        ConfState conf_state = capnp_util::make<msg::ConfState>();
        auto conf_builder = capnp_util::builder<msg::ConfState>(conf_state);
        auto voters = conf_builder.initVoters(peers.size());
        for (size_t i = 0; i < peers.size(); ++i) {
            voters.set(i, peers[i]);
        }

        HardState hard_state = capnp_util::make<msg::HardState>();
        auto hs_builder = capnp_util::builder<msg::HardState>(hard_state);
        hs_builder.setCommit(0);
        hs_builder.setTerm(0);
        hs_builder.setVote(0);

        RaftState raft_state;
        raft_state.hard_state = std::move(hard_state);
        raft_state.conf_state = std::move(conf_state);
        storage->SetRaftState(std::move(raft_state));
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
    if (initial_state) {
        auto conf_reader = capnp_util::reader<msg::ConfState>(initial_state->conf_state);
        if (conf_reader.getVoters().size() > 0) {
            if (peers.empty()) {
                throw std::runtime_error("NewTestRaft with empty peers on initialized store");
            }
        } else if (!peers.empty()) {
            // Initialize with conf state
            ConfState conf_state = capnp_util::make<msg::ConfState>();
            auto conf_builder = capnp_util::builder<msg::ConfState>(conf_state);
            auto voters = conf_builder.initVoters(peers.size());
            for (size_t i = 0; i < peers.size(); ++i) {
                voters.set(i, peers[i]);
            }

            HardState hard_state = capnp_util::make<msg::HardState>();
            auto hs_builder = capnp_util::builder<msg::HardState>(hard_state);
            hs_builder.setCommit(0);
            hs_builder.setTerm(0);
            hs_builder.setVote(0);

            RaftState raft_state;
            raft_state.hard_state = std::move(hard_state);
            raft_state.conf_state = std::move(conf_state);
            storage->SetRaftState(std::move(raft_state));
        }
    } else if (!peers.empty()) {
        // Initialize with conf state
        ConfState conf_state = capnp_util::make<msg::ConfState>();
        auto conf_builder = capnp_util::builder<msg::ConfState>(conf_state);
        auto voters = conf_builder.initVoters(peers.size());
        for (size_t i = 0; i < peers.size(); ++i) {
            voters.set(i, peers[i]);
        }

        HardState hard_state = capnp_util::make<msg::HardState>();
        auto hs_builder = capnp_util::builder<msg::HardState>(hard_state);
        hs_builder.setCommit(0);
        hs_builder.setTerm(0);
        hs_builder.setVote(0);

        RaftState raft_state;
        raft_state.hard_state = std::move(hard_state);
        raft_state.conf_state = std::move(conf_state);
        storage->SetRaftState(std::move(raft_state));
    }

    return NewTestRaftWithConfig(config, storage);
}

Interface NewTestRaftWithLogs(
    uint64_t id, const std::vector<uint64_t>& peers, size_t election, size_t heartbeat,
    std::shared_ptr<MemoryStorage> storage, const std::vector<Entry>& logs
) {
    Config config = NewTestConfig(id, election, heartbeat);

    auto initial_state = storage->InitialState();
    if (initial_state) {
        auto conf_reader = capnp_util::reader<msg::ConfState>(initial_state->conf_state);
        if (conf_reader.getVoters().size() > 0) {
            if (peers.empty()) {
                throw std::runtime_error("NewTestRaft with empty peers on initialized store");
            }
        } else if (!peers.empty()) {
            // Initialize with conf state
            ConfState conf_state = capnp_util::make<msg::ConfState>();
            auto conf_builder = capnp_util::builder<msg::ConfState>(conf_state);
            auto voters = conf_builder.initVoters(peers.size());
            for (size_t i = 0; i < peers.size(); ++i) {
                voters.set(i, peers[i]);
            }

            HardState hard_state = capnp_util::make<msg::HardState>();
            auto hs_builder = capnp_util::builder<msg::HardState>(hard_state);
            hs_builder.setCommit(0);
            hs_builder.setTerm(0);
            hs_builder.setVote(0);

            RaftState raft_state;
            raft_state.hard_state = std::move(hard_state);
            raft_state.conf_state = std::move(conf_state);
            storage->SetRaftState(std::move(raft_state));
        }
    } else if (!peers.empty()) {
        // Initialize with conf state
        ConfState conf_state = capnp_util::make<msg::ConfState>();
        auto conf_builder = capnp_util::builder<msg::ConfState>(conf_state);
        auto voters = conf_builder.initVoters(peers.size());
        for (size_t i = 0; i < peers.size(); ++i) {
            voters.set(i, peers[i]);
        }

        HardState hard_state = capnp_util::make<msg::HardState>();
        auto hs_builder = capnp_util::builder<msg::HardState>(hard_state);
        hs_builder.setCommit(0);
        hs_builder.setTerm(0);
        hs_builder.setVote(0);

        RaftState raft_state;
        raft_state.hard_state = std::move(hard_state);
        raft_state.conf_state = std::move(conf_state);
        storage->SetRaftState(std::move(raft_state));
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
        Snapshot snap = capnp_util::make<msg::Snapshot>();
        auto snap_builder = capnp_util::builder<msg::Snapshot>(snap);
        auto meta_builder = snap_builder.initMetadata();
        meta_builder.setIndex(snapshot_index);

        // Get term for snapshot index from storage
        auto term = storage->Term(snapshot_index);
        if (term) {
            meta_builder.setTerm(term.value());
        }

        // Copy conf state
        if (initial_state) {
            auto conf_src = capnp_util::reader<msg::ConfState>(initial_state->conf_state);
            auto conf_dst = meta_builder.initConfState();

            auto voters_src = conf_src.getVoters();
            auto voters_dst = conf_dst.initVoters(voters_src.size());
            for (size_t i = 0; i < voters_src.size(); ++i) {
                voters_dst.set(i, voters_src[i]);
            }

            auto learners_src = conf_src.getLearners();
            auto learners_dst = conf_dst.initLearners(learners_src.size());
            for (size_t i = 0; i < learners_src.size(); ++i) {
                learners_dst.set(i, learners_src[i]);
            }

            auto voters_out_src = conf_src.getVotersOutgoing();
            auto voters_out_dst = conf_dst.initVotersOutgoing(voters_out_src.size());
            for (size_t i = 0; i < voters_out_src.size(); ++i) {
                voters_out_dst.set(i, voters_out_src[i]);
            }

            auto learners_next_src = conf_src.getLearnersNext();
            auto learners_next_dst = conf_dst.initLearnersNext(learners_next_src.size());
            for (size_t i = 0; i < learners_next_src.size(); ++i) {
                learners_next_dst.set(i, learners_next_src[i]);
            }

            conf_dst.setAutoLeave(conf_src.getAutoLeave());
        }
        owned_storage->ApplySnapshot(snap).value();
    }

    // Set raft state - ensure commit is at least snapshot_index
    if (initial_state) {
        HardState hard_state = CloneHardState(initial_state->hard_state);
        // Commit should never be less than snapshot index
        auto hs_reader = capnp_util::reader<msg::HardState>(hard_state);
        if (hs_reader.getCommit() < snapshot_index) {
            auto hs_builder = capnp_util::builder<msg::HardState>(hard_state);
            hs_builder.setCommit(snapshot_index);
        }

        RaftState raft_state;
        raft_state.hard_state = std::move(hard_state);
        raft_state.conf_state = CloneConfState(initial_state->conf_state);
        owned_storage->SetRaftState(std::move(raft_state));
    }

    // Copy entries from shared storage
    auto entries = storage->AllEntries();
    if (!entries.empty()) {
        owned_storage->Append(entries).value();
    }

    auto raft = std::make_unique<Raft>(config, std::move(owned_storage));
    return Interface(std::move(raft), storage);
}

SoftState MakeSoftState(const uint64_t leader_id, const StateRole state) {
    return SoftState{leader_id, state};
}

RaftState MakeRaftState(const HardState& hs, const ConfState& cs) {
    RaftState rs;
    rs.hard_state = CloneHardState(hs);
    rs.conf_state = CloneConfState(cs);
    return rs;
}

Message NewMessageWithEntries(
    const uint64_t from, const uint64_t to, const MessageType type, std::vector<Entry> entries
) {
    Message m = capnp_util::make<msg::Message>();
    auto builder = capnp_util::builder<msg::Message>(m);
    builder.setMsgType(type);
    builder.setTo(to);
    builder.setFrom(from);

    auto entries_builder = builder.initEntries(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        entries_builder.setWithCaveats(i, capnp_util::reader<msg::Entry>(entries[i]));
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

Entry NewEntry(
    const uint64_t index, const uint64_t term, const std::optional<std::string>& data,
    const std::optional<std::string>& context
) {
    Entry e = capnp_util::make<msg::Entry>();
    auto builder = capnp_util::builder<msg::Entry>(e);
    builder.setIndex(index);
    builder.setTerm(term);
    if (data.has_value()) {
        builder.setData(::capnp::Data::Reader(
            reinterpret_cast<const ::capnp::byte*>(data->data()), data->size()
        ));
        // Always set context when data is set, to match Raft's internal behavior
        // (HandleAppendEntries always calls setContext, even if empty)
        if (context.has_value()) {
            builder.setContext(::capnp::Data::Reader(
                reinterpret_cast<const ::capnp::byte*>(context->data()), context->size()
            ));
        } else {
            // Set empty context to match Raft's behavior
            builder.setContext(::capnp::Data::Reader(nullptr, 0));
        }
    } else if (context.has_value()) {
        builder.setContext(::capnp::Data::Reader(
            reinterpret_cast<const ::capnp::byte*>(context->data()), context->size()
        ));
    }
    return e;
}

Entry EmptyEntry(const uint64_t index, const uint64_t term) {
    return NewEntry(index, term, std::nullopt, std::nullopt);
}

Snapshot NewSnapshot(
    const uint64_t index, const uint64_t term, const std::vector<uint64_t>& voters
) {
    Snapshot snap = capnp_util::make<msg::Snapshot>();
    auto builder = capnp_util::builder<msg::Snapshot>(snap);
    // Set empty data to match Raft's internal behavior
    // (HandleSnapshot always calls setData, even if empty)
    builder.setData(::capnp::Data::Reader(nullptr, 0));

    auto meta_builder = builder.initMetadata();
    meta_builder.setIndex(index);
    meta_builder.setTerm(term);

    auto conf_builder = meta_builder.initConfState();
    auto voters_builder = conf_builder.initVoters(voters.size());
    for (size_t i = 0; i < voters.size(); ++i) {
        voters_builder.set(i, voters[i]);
    }
    // Initialize empty lists to match Raft's internal behavior
    // (HandleSnapshot always calls init* on all lists)
    conf_builder.initLearners(0);
    conf_builder.initVotersOutgoing(0);
    conf_builder.initLearnersNext(0);
    conf_builder.setAutoLeave(false);
    return snap;
}

ConfChange MakeConfChange(const ConfChangeType type, const uint64_t node_id) {
    ConfChange cc = capnp_util::make<msg::ConfChange>();
    auto builder = capnp_util::builder<msg::ConfChange>(cc);
    builder.setChangeType(type);
    builder.setNodeId(node_id);
    return cc;
}

ConfChangeV2 MakeRemoveNodeCC(const uint64_t node_id) {
    ConfChangeV2 cc = capnp_util::make<msg::ConfChangeV2>();
    auto builder = capnp_util::builder<msg::ConfChangeV2>(cc);
    auto changes = builder.initChanges(1);
    changes[0].setChangeType(ConfChangeType::REMOVE_NODE);
    changes[0].setNodeId(node_id);
    return cc;
}

ConfChangeV2 MakeAddNodeCC(const uint64_t node_id) {
    ConfChangeV2 cc = capnp_util::make<msg::ConfChangeV2>();
    auto builder = capnp_util::builder<msg::ConfChangeV2>(cc);
    auto changes = builder.initChanges(1);
    changes[0].setChangeType(ConfChangeType::ADD_NODE);
    changes[0].setNodeId(node_id);
    return cc;
}

ConfChangeV2 MakeAddLearnerCC(const uint64_t node_id) {
    ConfChangeV2 cc = capnp_util::make<msg::ConfChangeV2>();
    auto builder = capnp_util::builder<msg::ConfChangeV2>(cc);
    auto changes = builder.initChanges(1);
    changes[0].setChangeType(ConfChangeType::ADD_LEARNER_NODE);
    changes[0].setNodeId(node_id);
    return cc;
}

ConfChangeV2 MakeConfChangeV2Single(const ConfChangeType type, const uint64_t node_id) {
    ConfChangeV2 cc = capnp_util::make<msg::ConfChangeV2>();
    auto builder = capnp_util::builder<msg::ConfChangeV2>(cc);
    auto changes = builder.initChanges(1);
    changes[0].setChangeType(type);
    changes[0].setNodeId(node_id);
    return cc;
}

ConfState MakeConfState(
    const std::vector<uint64_t>& voters, const std::vector<uint64_t>& learners
) {
    ConfState cs = capnp_util::make<msg::ConfState>();
    auto builder = capnp_util::builder<msg::ConfState>(cs);

    auto voters_builder = builder.initVoters(voters.size());
    for (size_t i = 0; i < voters.size(); ++i) {
        voters_builder.set(i, voters[i]);
    }

    auto learners_builder = builder.initLearners(learners.size());
    for (size_t i = 0; i < learners.size(); ++i) {
        learners_builder.set(i, learners[i]);
    }

    return cs;
}

std::string LogToString(const RaftLog& raft_log) {
    std::string s = fmt::format("committed: {}\n", raft_log.committed());
    s += fmt::format("applied: {}\n", raft_log.applied());

    auto entries = const_cast<RaftLog&>(raft_log).AllEntries();
    for (size_t i = 0; i < entries.size(); ++i) {
        auto reader = capnp_util::reader<msg::Entry>(entries[i]);
        s += fmt::format("#{}: index={} term={}\n", i, reader.getIndex(), reader.getTerm());
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
    Snapshot snap = capnp_util::make<msg::Snapshot>();
    auto snap_builder = capnp_util::builder<msg::Snapshot>(snap);
    auto meta_builder = snap_builder.initMetadata();
    meta_builder.setIndex(0);
    meta_builder.setTerm(0);

    auto conf_builder = meta_builder.initConfState();
    auto voters_builder = conf_builder.initVoters(peers.size());
    for (size_t i = 0; i < peers.size(); ++i) {
        voters_builder.set(i, peers[i]);
    }
    storage->ApplySnapshot(snap).value();

    // Append entries
    std::vector<Entry> entries;
    for (size_t i = 0; i < terms.size(); ++i) {
        Entry e = capnp_util::make<msg::Entry>();
        auto e_builder = capnp_util::builder<msg::Entry>(e);
        e_builder.setIndex(i + 1);
        e_builder.setTerm(terms[i]);
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
    Snapshot snap = capnp_util::make<msg::Snapshot>();
    auto snap_builder = capnp_util::builder<msg::Snapshot>(snap);
    auto meta_builder = snap_builder.initMetadata();
    meta_builder.setIndex(0);
    meta_builder.setTerm(0);

    auto conf_builder = meta_builder.initConfState();
    auto voters_builder = conf_builder.initVoters(peers.size());
    for (size_t i = 0; i < peers.size(); ++i) {
        voters_builder.set(i, peers[i]);
    }
    storage->ApplySnapshot(snap).value();

    // Set hard state with vote and term
    HardState hs = capnp_util::make<msg::HardState>();
    auto hs_builder = capnp_util::builder<msg::HardState>(hs);
    hs_builder.setVote(vote);
    hs_builder.setTerm(term);
    hs_builder.setCommit(0);

    RaftState rs;
    rs.hard_state = std::move(hs);
    rs.conf_state = capnp_util::make<msg::ConfState>();
    storage->SetRaftState(std::move(rs));

    return NewTestRaftWithPrevote(id, {}, 5, 1, storage, pre_vote);
}

std::vector<Entry> NextEntries(Raft& r, MemoryStorage& s) {
    auto& raft_log = r.raft_log();

    // Persist unstable entries
    const auto& unstable_entries = raft_log.unstable().entries();
    if (!unstable_entries.empty()) {
        // Clone entries BEFORE calling StableEntries, which clears them
        std::vector<Entry> entries_to_persist;
        entries_to_persist.reserve(unstable_entries.size());
        for (const auto& entry : unstable_entries) {
            entries_to_persist.push_back(CloneEntry(entry));
        }

        const auto& last_entry = entries_to_persist.back();
        auto last_reader = capnp_util::reader<msg::Entry>(last_entry);
        const uint64_t last_idx = last_reader.getIndex();
        const uint64_t last_term = last_reader.getTerm();

        // First append to storage, then mark as stable
        s.Append(entries_to_persist).value();

        // Also update the internal storage
        auto* internal_storage = dynamic_cast<MemoryStorage*>(raft_log.storage());
        if (internal_storage) {
            std::vector<Entry> internal_copy;
            internal_copy.reserve(entries_to_persist.size());
            for (const auto& entry : entries_to_persist) {
                internal_copy.push_back(CloneEntry(entry));
            }
            internal_storage->Append(internal_copy).value();
        }

        // Now mark as stable
        raft_log.StableEntries(last_idx, last_term);
        r.OnPersistEntries(last_idx, last_term);
    }

    // Get next entries
    auto ents = raft_log.NextEntries(std::nullopt);
    r.CommitApply(raft_log.committed());
    if (ents.has_value()) {
        return std::move(*ents);
    }
    return std::vector<Entry>{};
}

void CommitNoopEntry(Network& /*network*/, MemoryStorage& storage, Raft& raft) {
    // This helper commits the initial no-op entry after leader election
    // by having the leader broadcast and receive responses

    // First append a no-op entry to make LastIndex > 0
    // This ensures BroadcastAppend will send entries
    Entry noop_entry = capnp_util::make<msg::Entry>();
    auto noop_builder = capnp_util::builder<msg::Entry>(noop_entry);
    noop_builder.setTerm(raft.term());
    noop_builder.setIndex(raft.raft_log().LastIndex() + 1);
    std::ignore = raft.AppendEntry(noop_entry);

    // Now broadcast append messages (which will include the no-op entry)
    raft.BroadcastAppend();

    auto& msgs = raft.messages();
    std::vector<Message> msgs_copy;
    for (auto& msg : msgs) {
        msgs_copy.push_back(CloneMessage(msg));
    }
    raft.messages().clear();

    for (auto& msg : msgs_copy) {
        auto msg_reader = capnp_util::reader<msg::Message>(msg);
        if (msg_reader.getMsgType() == MessageType::MSG_APPEND) {
            // Create response
            Message resp = capnp_util::make<msg::Message>();
            auto resp_builder = capnp_util::builder<msg::Message>(resp);
            resp_builder.setMsgType(MessageType::MSG_APPEND_RESPONSE);
            resp_builder.setFrom(msg_reader.getTo());
            resp_builder.setTo(msg_reader.getFrom());
            resp_builder.setTerm(raft.term());
            resp_builder.setIndex(msg_reader.getIndex() + msg_reader.getEntries().size());

            std::ignore = raft.Step(resp);
        }
    }

    // Persist and commit
    raftpp::NextEntries(raft, storage);
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
    bool is_initialized = initial_state &&
        capnp_util::reader<msg::ConfState>(initial_state->conf_state).getVoters().size() > 0;

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
    bool is_initialized = initial_state &&
        capnp_util::reader<msg::ConfState>(initial_state->conf_state).getVoters().size() > 0;

    // If storage is already initialized, just use it as-is (empty peers means use existing config)
    // If storage is NOT initialized and peers is provided, initialize with snapshot
    if (!is_initialized && !peers.empty()) {
        storage->ApplySnapshot(NewSnapshot(1, 1, peers)).value();
    }

    return RawNode(config, std::move(storage));
}

// Entry comparison - uses value equality via Cap'n Proto
bool EntryEquals(const Entry& e1, const Entry& e2) {
    return capnp_util::equal<msg::Entry>(
        capnp_util::reader<msg::Entry>(e1), capnp_util::reader<msg::Entry>(e2)
    );
}

// HardState comparison - uses value equality via Cap'n Proto
bool HardStateEquals(const HardState& e1, const HardState& e2) {
    return capnp_util::equal<msg::HardState>(
        capnp_util::reader<msg::HardState>(e1), capnp_util::reader<msg::HardState>(e2)
    );
}

// Snapshot comparison - uses value equality via Cap'n Proto
bool SnapshotEquals(const Snapshot& e1, const Snapshot& e2) {
    if (!e1 || !e2) {
        return !e1 && !e2;
    }
    return capnp_util::equal<msg::Snapshot>(
        capnp_util::reader<msg::Snapshot>(e1), capnp_util::reader<msg::Snapshot>(e2)
    );
}

bool operator==(const std::optional<HardState>& e1, const std::optional<HardState>& e2) {
    if (e1.has_value() != e2.has_value()) {
        return false;
    }
    if (!e1.has_value() && !e2.has_value()) {
        return true;
    }
    return HardStateEquals(*e1, *e2);
}

bool operator==(const std::vector<Entry>& e1, const std::vector<Entry>& e2) {
    if (e1.size() != e2.size()) {
        return false;
    }

    for (size_t i = 0; i < e1.size(); ++i) {
        if (!EntryEquals(e1.at(i), e2.at(i))) {
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
        return SnapshotEquals(*e1, *e2);
    }
    return e1.error() == e2.error();
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

bool ConfStateEquals(const ConfState& e1, const ConfState& e2) {
    // Compare as sets because the order of voters/learners may differ
    auto r1 = capnp_util::reader<msg::ConfState>(e1);
    auto r2 = capnp_util::reader<msg::ConfState>(e2);

    // Helper to convert Cap'n Proto list to set
    auto toSet = [](auto list) {
        std::set<uint64_t> result;
        for (auto item : list) {
            result.insert(item);
        }
        return result;
    };

    // Compare voters
    if (toSet(r1.getVoters()) != toSet(r2.getVoters())) {
        return false;
    }

    // Compare learners
    if (toSet(r1.getLearners()) != toSet(r2.getLearners())) {
        return false;
    }

    // Compare voters_outgoing
    if (toSet(r1.getVotersOutgoing()) != toSet(r2.getVotersOutgoing())) {
        return false;
    }

    // Compare learners_next
    if (toSet(r1.getLearnersNext()) != toSet(r2.getLearnersNext())) {
        return false;
    }

    // Compare auto_leave
    if (r1.getAutoLeave() != r2.getAutoLeave()) {
        return false;
    }

    return true;
}

void MustCmpReady(
    const Ready& rd, const std::optional<SoftState>& ss, const std::optional<HardState>& hs,
    const std::vector<Entry>& entries, const std::vector<Entry>& committed_entries,
    const std::optional<Snapshot>& snapshot, bool msg_is_empty, bool persisted_msg_is_empty,
    bool must_sync
) {
    CHECK_EQ(rd.ss, ss);
    CHECK(raftpp::operator==(rd.hs, hs));
    CHECK(raftpp::operator==(rd.entries, entries));
    CHECK(raftpp::operator==(rd.light.committed_entries, committed_entries));
    CHECK_EQ(rd.read_states.empty(), true);

    if (snapshot.has_value()) {
        CHECK(rd.snapshot);
        CHECK(SnapshotEquals(rd.snapshot, *snapshot));
    } else {
        if (rd.snapshot) {
            Snapshot default_snap = capnp_util::make<msg::Snapshot>();
            CHECK(SnapshotEquals(rd.snapshot, default_snap));
        }
    }

    CHECK_EQ(rd.Messages().empty(), msg_is_empty);
    CHECK_EQ(rd.light.messages.empty(), persisted_msg_is_empty);
    CHECK_EQ(rd.must_sync, must_sync);
}

ConfState MakeConfStateV2(
    const std::vector<uint64_t>& voters, const std::vector<uint64_t>& learners,
    const std::vector<uint64_t>& voters_outgoing, const std::vector<uint64_t>& learners_next,
    const bool auto_leave
) {
    ConfState cs = capnp_util::make<msg::ConfState>();
    auto builder = capnp_util::builder<msg::ConfState>(cs);

    auto voters_builder = builder.initVoters(voters.size());
    for (size_t i = 0; i < voters.size(); ++i) {
        voters_builder.set(i, voters[i]);
    }

    auto learners_builder = builder.initLearners(learners.size());
    for (size_t i = 0; i < learners.size(); ++i) {
        learners_builder.set(i, learners[i]);
    }

    auto voters_out_builder = builder.initVotersOutgoing(voters_outgoing.size());
    for (size_t i = 0; i < voters_outgoing.size(); ++i) {
        voters_out_builder.set(i, voters_outgoing[i]);
    }

    auto learners_next_builder = builder.initLearnersNext(learners_next.size());
    for (size_t i = 0; i < learners_next.size(); ++i) {
        learners_next_builder.set(i, learners_next[i]);
    }

    builder.setAutoLeave(auto_leave);
    return cs;
}

}  // namespace raftpp
