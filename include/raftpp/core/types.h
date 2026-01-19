#pragma once

// This header provides wrapper types to ease the migration from Protobuf to Cap'n Proto.
// The wrappers provide a Protobuf-like API while using Cap'n Proto underneath.

#include "capnp_message.h"
#include "raftpp.capnp.h"

namespace raftpp {

// For now, directly use OwnedMessage as type aliases.
// Code will need to be updated to use builder()/reader() methods.
// This is the cleanest approach that leverages Cap'n Proto's zero-copy design.

using Entry = OwnedMessage<raftpp::capnp::Entry>;
using Snapshot = OwnedMessage<raftpp::capnp::Snapshot>;
using SnapshotMetadata = OwnedMessage<raftpp::capnp::SnapshotMetadata>;
using Message = OwnedMessage<raftpp::capnp::Message>;
using HardState = OwnedMessage<raftpp::capnp::HardState>;
using ConfState = OwnedMessage<raftpp::capnp::ConfState>;
using ConfChange = OwnedMessage<raftpp::capnp::ConfChange>;
using ConfChangeSingle = OwnedMessage<raftpp::capnp::ConfChangeSingle>;
using ConfChangeV2 = OwnedMessage<raftpp::capnp::ConfChangeV2>;

// Enum type aliases (these are directly usable from Cap'n Proto)
using EntryType = raftpp::capnp::EntryType;
using MessageType = raftpp::capnp::MessageType;
using ConfChangeType = raftpp::capnp::ConfChangeType;
using ConfChangeTransition = raftpp::capnp::ConfChangeTransition;

// Helper functions for common operations

// Create a new Entry
inline Entry MakeEntry(uint64_t index, uint64_t term, EntryType type = EntryType::ENTRY_NORMAL) {
    return makeMessage<raftpp::capnp::Entry>([&](auto builder) {
        builder.setIndex(index);
        builder.setTerm(term);
        builder.setEntryType(type);
    });
}

// Create a new HardState
inline HardState MakeHardState(uint64_t term, uint64_t vote, uint64_t commit) {
    return makeMessage<raftpp::capnp::HardState>([&](auto builder) {
        builder.setTerm(term);
        builder.setVote(vote);
        builder.setCommit(commit);
    });
}

// Check if HardState is empty (all zeros)
inline bool IsEmptyHardState(const HardState& hs) {
    auto reader = hs.reader();
    return reader.getTerm() == 0 && reader.getVote() == 0 && reader.getCommit() == 0;
}

// Check if ConfState is equivalent
inline bool IsConfStateEquivalent(const ConfState& a, const ConfState& b) {
    return messagesEqual<raftpp::capnp::ConfState>(a.reader(), b.reader());
}

}  // namespace raftpp
