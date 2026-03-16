#pragma once

// This header provides types for working with Cap'n Proto messages.
// Uses unique_ptr<MallocMessageBuilder> directly for zero-copy efficiency.

#include <memory>

#include <capnp/message.h>

#include "capnp_util.h"
#include "raftpp.capnp.h"

namespace raftpp {

// Ownership type - unique_ptr to MallocMessageBuilder
// The template parameter is not enforced at the type level, but serves as documentation.
template <typename T>
using OwnedBuilder = std::unique_ptr<::capnp::MallocMessageBuilder>;

// Namespace for Cap'n Proto message types
namespace msg {
using Entry = capnp::Entry;
using Snapshot = capnp::Snapshot;
using SnapshotMetadata = capnp::SnapshotMetadata;
using Message = capnp::Message;
using HardState = capnp::HardState;
using ConfState = capnp::ConfState;
using ConfChange = capnp::ConfChange;
using ConfChangeSingle = capnp::ConfChangeSingle;
using ConfChangeV2 = capnp::ConfChangeV2;
}  // namespace msg

// Main type aliases - use unique_ptr for direct ownership
using Entry = OwnedBuilder<msg::Entry>;
using Snapshot = OwnedBuilder<msg::Snapshot>;
using SnapshotMetadata = OwnedBuilder<msg::SnapshotMetadata>;
using Message = OwnedBuilder<msg::Message>;
using HardState = OwnedBuilder<msg::HardState>;
using ConfState = OwnedBuilder<msg::ConfState>;
using ConfChange = OwnedBuilder<msg::ConfChange>;
using ConfChangeSingle = OwnedBuilder<msg::ConfChangeSingle>;
using ConfChangeV2 = OwnedBuilder<msg::ConfChangeV2>;

// Enum type aliases (these are directly usable from Cap'n Proto)
using EntryType = capnp::EntryType;
using MessageType = capnp::MessageType;
using ConfChangeType = capnp::ConfChangeType;
using ConfChangeTransition = capnp::ConfChangeTransition;

// Helper functions for common operations

// Create a new Entry
inline Entry MakeEntry(uint64_t index, uint64_t term, EntryType type = EntryType::ENTRY_NORMAL) {
    return capnp_util::make<msg::Entry>([&](auto builder) {
        builder.setIndex(index);
        builder.setTerm(term);
        builder.setEntryType(static_cast<::raftpp::capnp::EntryType>(static_cast<int>(type)));
    });
}

// Create a new HardState
inline HardState MakeHardState(uint64_t term, uint64_t vote, uint64_t commit) {
    return capnp_util::make<msg::HardState>([&](auto builder) {
        builder.setTerm(term);
        builder.setVote(vote);
        builder.setCommit(commit);
    });
}

// Check if HardState is empty (all zeros)
inline bool IsEmptyHardState(const HardState& hs) {
    auto reader = capnp_util::reader<msg::HardState>(hs);
    return reader.getTerm() == 0 && reader.getVote() == 0 && reader.getCommit() == 0;
}

// Check if ConfState is equivalent
inline bool IsConfStateEquivalent(const ConfState& a, const ConfState& b) {
    return capnp_util::equal<msg::ConfState>(
        capnp_util::reader<msg::ConfState>(a), capnp_util::reader<msg::ConfState>(b)
    );
}

// Clone an Entry
inline Entry CloneEntry(const Entry& e) {
    return capnp_util::clone<msg::Entry>(e);
}

// Clone a HardState
inline HardState CloneHardState(const HardState& hs) {
    return capnp_util::clone<msg::HardState>(hs);
}

// Clone a ConfState
inline ConfState CloneConfState(const ConfState& cs) {
    return capnp_util::clone<msg::ConfState>(cs);
}

// Clone a Snapshot
inline Snapshot CloneSnapshot(const Snapshot& s) {
    return capnp_util::clone<msg::Snapshot>(s);
}

// Clone a Message
inline Message CloneMessage(const Message& m) {
    return capnp_util::clone<msg::Message>(m);
}

// Clone a ConfChangeV2
inline ConfChangeV2 CloneConfChangeV2(const ConfChangeV2& cc) {
    return capnp_util::clone<msg::ConfChangeV2>(cc);
}

// Clone a ConfChangeSingle
inline ConfChangeSingle CloneConfChangeSingle(const ConfChangeSingle& cc) {
    return capnp_util::clone<msg::ConfChangeSingle>(cc);
}

}  // namespace raftpp
