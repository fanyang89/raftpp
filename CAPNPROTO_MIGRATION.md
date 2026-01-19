# Cap'n Proto Migration Guide

## Overview

This document provides detailed guidance for migrating raftpp from Protobuf to Cap'n Proto. Phase 1 (build system and type infrastructure) is complete. This guide covers the remaining work.

## Table of Contents

1. [API Changes](#api-changes)
2. [Common Patterns](#common-patterns)
3. [File-by-File Migration Checklist](#file-by-file-migration-checklist)
4. [Testing Strategy](#testing-strategy)
5. [Troubleshooting](#troubleshooting)

---

## API Changes

### Fundamental Difference: Mutable Messages vs Builder/Reader Pattern

**Protobuf**: Messages are mutable objects
```cpp
Entry entry;
entry.set_term(5);
entry.set_index(10);
uint64_t term = entry.term();
```

**Cap'n Proto**: Messages use builder/reader pattern
```cpp
Entry entry;  // entry is OwnedMessage<raftpp::capnp::Entry>
entry.builder().setTerm(5);
entry.builder().setIndex(10);
uint64_t term = entry.reader().getTerm();
```

### Field Access Patterns

| Operation | Protobuf | Cap'n Proto |
|-----------|----------|-------------|
| Read field | `msg.term()` | `msg.reader().getTerm()` |
| Write field | `msg.set_term(5)` | `msg.builder().setTerm(5)` |
| Has field | `msg.has_snapshot()` | `msg.reader().hasSnapshot()` |
| Clear field | `msg.clear_term()` | `msg.builder().setTerm(0)` or rebuild |
| Mutable access | `msg.mutable_snapshot()` | `msg.builder().initSnapshot()` |

### Enum Values

**Protobuf**: `EntryType::EntryNormal`
**Cap'n Proto**: `EntryType::ENTRY_NORMAL`

All enum values are UPPER_SNAKE_CASE in Cap'n Proto.

| Protobuf Enum | Cap'n Proto Enum |
|---------------|------------------|
| `EntryType::EntryNormal` | `EntryType::ENTRY_NORMAL` |
| `EntryType::EntryConfChange` | `EntryType::ENTRY_CONF_CHANGE` |
| `MessageType::MsgHup` | `MessageType::MSG_HUP` |
| `MessageType::MsgAppend` | `MessageType::MSG_APPEND` |
| `ConfChangeType::AddNode` | `ConfChangeType::ADD_NODE` |

### Repeated Fields (Lists)

**Protobuf**:
```cpp
// Read
for (const auto& entry : msg.entries()) {
    process(entry);
}

// Write
auto* new_entry = msg.add_entries();
new_entry->set_term(5);

// Or
msg.mutable_entries()->Add(entry);
```

**Cap'n Proto**:
```cpp
// Read
auto entries = msg.reader().getEntries();
for (const auto& entry : entries) {
    uint64_t term = entry.getTerm();
    process(term);
}

// Write - must know size upfront
auto builder = msg.builder();
auto entries = builder.initEntries(count);
entries[0].setTerm(5);
entries[0].setIndex(1);

// Or copy from vector
std::vector<Entry> entries_vec = ...;
auto builder = msg.builder();
auto entries = builder.initEntries(entries_vec.size());
for (size_t i = 0; i < entries_vec.size(); ++i) {
    entries[i] = entries_vec[i].reader();
}
```

### Bytes Fields

**Protobuf**:
```cpp
msg.set_data("hello");
std::string data = msg.data();
```

**Cap'n Proto**:
```cpp
auto builder = msg.builder();
builder.setData(kj::StringPtr("hello").asBytes());

auto reader = msg.reader();
auto data = reader.getData();  // Returns capnp::Data::Reader
std::string str(reinterpret_cast<const char*>(data.begin()), data.size());
```

### Message Comparison

**Protobuf**:
```cpp
if (msg1 == msg2) { ... }
// Or
google::protobuf::util::MessageDifferencer::Equals(msg1, msg2)
```

**Cap'n Proto**:
```cpp
// Use helper from types.h
if (messagesEqual<raftpp::capnp::Message>(msg1.reader(), msg2.reader())) { ... }
```

### Serialization

**Protobuf**:
```cpp
std::string serialized = msg.SerializeAsString();
Entry msg2;
msg2.ParseFromString(serialized);
```

**Cap'n Proto**:
```cpp
std::string serialized = msg.serializeAsString();
Entry msg2 = Entry::parseFromString(serialized);
```

### Message Size

**Protobuf**:
```cpp
size_t size = msg.ByteSizeLong();
```

**Cap'n Proto**:
```cpp
size_t size = msg.serializeAsBytes().size();
```

---

## Common Patterns

### Pattern 1: Creating a New Message

**Before (Protobuf)**:
```cpp
Entry entry;
entry.set_term(5);
entry.set_index(10);
entry.set_entry_type(EntryType::EntryNormal);
```

**After (Cap'n Proto)**:
```cpp
// Option 1: Using helper
Entry entry = MakeEntry(10, 5, EntryType::ENTRY_NORMAL);

// Option 2: Manual
Entry entry;
auto builder = entry.builder();
builder.setTerm(5);
builder.setIndex(10);
builder.setEntryType(EntryType::ENTRY_NORMAL);

// Option 3: Using makeMessage helper
Entry entry = makeMessage<raftpp::capnp::Entry>([](auto builder) {
    builder.setTerm(5);
    builder.setIndex(10);
    builder.setEntryType(EntryType::ENTRY_NORMAL);
});
```

### Pattern 2: Copying Message Fields

**Before (Protobuf)**:
```cpp
void UpdateMessage(Message& dest, const Message& src) {
    dest.set_term(src.term());
    dest.set_from(src.from());
    dest.set_to(src.to());
}
```

**After (Cap'n Proto)**:
```cpp
void UpdateMessage(Message& dest, const Message& src) {
    auto dest_builder = dest.builder();
    auto src_reader = src.reader();
    dest_builder.setTerm(src_reader.getTerm());
    dest_builder.setFrom(src_reader.getFrom());
    dest_builder.setTo(src_reader.getTo());
}

// Or for full copy:
Message dest = src.clone();
```

### Pattern 3: Building Messages with Lists

**Before (Protobuf)**:
```cpp
Message msg;
msg.set_type(MessageType::MsgAppend);
for (const Entry& ent : entries) {
    *msg.add_entries() = ent;
}
```

**After (Cap'n Proto)**:
```cpp
Message msg;
auto builder = msg.builder();
builder.setMsgType(MessageType::MSG_APPEND);

auto entries_builder = builder.initEntries(entries.size());
for (size_t i = 0; i < entries.size(); ++i) {
    entries_builder.setWithCaveats(i, entries[i].reader());
}
```

### Pattern 4: Checking Empty/Default Values

**Before (Protobuf)**:
```cpp
bool IsEmptyHardState(const HardState& hs) {
    return hs.term() == 0 && hs.vote() == 0 && hs.commit() == 0;
}
```

**After (Cap'n Proto)**:
```cpp
// Use helper from types.h
bool is_empty = IsEmptyHardState(hs);

// Or manually
auto reader = hs.reader();
bool is_empty = reader.getTerm() == 0 &&
                reader.getVote() == 0 &&
                reader.getCommit() == 0;
```

### Pattern 5: Working with Snapshots

**Before (Protobuf)**:
```cpp
Snapshot snapshot;
snapshot.set_data(snapshot_data);
auto* metadata = snapshot.mutable_metadata();
metadata->set_index(last_index);
metadata->set_term(last_term);
```

**After (Cap'n Proto)**:
```cpp
Snapshot snapshot;
auto builder = snapshot.builder();
builder.setData(kj::arrayPtr(
    reinterpret_cast<const kj::byte*>(snapshot_data.data()),
    snapshot_data.size()
));

auto metadata = builder.initMetadata();
metadata.setIndex(last_index);
metadata.setTerm(last_term);
```

---

## File-by-File Migration Checklist

### Core Implementation Files (lib/core/)

#### ✅ High Priority - Core Raft Logic

- [ ] **raft.cc** - Main Raft state machine
  - Update Step() message handling
  - Update AppendEntry, RequestVote message creation
  - Update all field accesses in step functions
  - Estimated changes: ~200 lines

- [ ] **raft_core.cc** - Core Raft functionality
  - Update message processing
  - Update state transitions
  - Estimated changes: ~150 lines

- [ ] **raw_node.cc** - User-facing API
  - Update Ready struct handling
  - Update Propose, ReadIndex methods
  - Estimated changes: ~100 lines

- [ ] **raft_log.cc** - Log management
  - Update Entry access patterns
  - Update Snapshot handling
  - Estimated changes: ~80 lines

#### ✅ Medium Priority - Configuration & Storage

- [ ] **conf_changer.cc** - Configuration changes
  - Update ConfChange, ConfChangeV2 handling
  - Update ConfState manipulation
  - Estimated changes: ~60 lines

- [ ] **memory_storage.cc** - In-memory storage
  - Update Entry vector operations
  - Update Snapshot storage
  - Estimated changes: ~50 lines

- [ ] **storage.cc** - Storage interface
  - Update base implementations
  - Estimated changes: ~20 lines

- [ ] **unstable_log.cc** - Unstable log buffer
  - Update Entry handling
  - Estimated changes: ~40 lines

#### ✅ Low Priority - Utilities

- [ ] **util.cc** - Utility functions
  - Remove Protobuf equality operators
  - Update EntryApproximateSize
  - Update IsContinuousEntries
  - Estimated changes: ~30 lines

- [ ] **progress.cc**, **progress_tracker.cc** - Progress tracking
  - Update Message handling in inflights
  - Estimated changes: ~20 lines each

- [ ] **read_only.cc** - Read-only queries
  - Update Message handling
  - Estimated changes: ~15 lines

### WAL Storage Layer (lib/raftor/wal/)

- [ ] **wal.cc** - Write-ahead log
  - Update Entry serialization
  - Update HardState persistence
  - Replace Protobuf SerializeToArray with Cap'n Proto
  - Estimated changes: ~80 lines

- [ ] **metadata_store.cc** - Metadata persistence
  - Update HardState/ConfState serialization
  - Estimated changes: ~40 lines

### RPC Transport Layer (lib/raftor/rpc/)

- [x] **Create capnp_transport.cc** - New Cap'n Proto RPC transport
  - Implement RaftTransport interface
  - Use Cap'n Proto RPC (EzRpcClient/Server)
  - Estimated: ~300 new lines

- [x] **Remove rpclib_transport.cc** - Old rpclib transport
- [ ] **Remove codec.cc** - Old codec (still used for framing)

### Test Files

- [x] **tests/*.cc** - Unit tests
  - Update all message creation
  - Update assertions
  - Estimated: ~500 lines across all tests

- [x] **tests/datadriven/*.cc** - Data-driven tests
  - Update message parsing
  - Estimated: ~200 lines

---

## Testing Strategy

### Phase 1: Unit Tests
1. Update and run individual unit tests incrementally
2. Start with basic tests (storage, log, etc.)
3. Progress to complex tests (raft state machine)

### Phase 2: Integration Tests
1. Run data-driven tests
2. Verify protocol compatibility

### Phase 3: Manual Testing
1. Start multi-node cluster
2. Verify message passing
3. Test configuration changes
4. Test snapshot transfer

---

## Troubleshooting

### Common Compilation Errors

**Error**: `no member named 'term' in 'raftpp::Entry'`
```cpp
// Wrong
uint64_t t = entry.term();

// Correct
uint64_t t = entry.reader().getTerm();
```

**Error**: `no member named 'set_term' in 'raftpp::Entry'`
```cpp
// Wrong
entry.set_term(5);

// Correct
entry.builder().setTerm(5);
```

**Error**: `cannot convert 'capnp::Data::Reader' to 'std::string'`
```cpp
// Wrong
std::string data = entry.data();

// Correct
auto data_reader = entry.reader().getData();
std::string data(reinterpret_cast<const char*>(data_reader.begin()),
                 data_reader.size());
```

**Error**: `list assignment requires size`
```cpp
// Wrong
msg.mutable_entries()->Add(entry);

// Correct - must initialize with size
auto entries = msg.builder().initEntries(1);
entries[0] = entry.reader();
```

### Runtime Issues

**Issue**: Segfault when accessing message fields
- **Cause**: Accessing builder/reader after message is destroyed
- **Solution**: Ensure message lifetime outlasts builder/reader usage

**Issue**: Data corruption in serialized messages
- **Cause**: Incorrect byte conversion or alignment
- **Solution**: Use provided serialization helpers (serializeAsBytes, parseFromBytes)

**Issue**: Performance degradation
- **Cause**: Excessive serialization for size calculation
- **Solution**: Cache serialized size if needed multiple times

---

## Migration Workflow

### Recommended Order:

1. **Start with leaf dependencies** (util.cc, storage.cc)
2. **Move to core components** (raft_log.cc, memory_storage.cc)
3. **Update main state machine** (raft.cc, raft_core.cc)
4. **Update public API** (raw_node.cc)
5. **Migrate WAL** (wal.cc, metadata_store.cc)
6. **Create new transport** (capnp_transport.cc)
7. **Update tests** (incrementally as you go)
8. **Final cleanup** (remove old code, run full test suite)

### After Each File:

1. Attempt compilation: `cmake --build build --target raftpp`
2. Fix compilation errors
3. Run related tests (if updated)
4. Commit with descriptive message

---

## Code Examples Reference

### Reading Message Fields

```cpp
void ProcessMessage(const Message& msg) {
    auto reader = msg.reader();

    // Basic fields
    uint64_t from = reader.getFrom();
    uint64_t to = reader.getTo();
    uint64_t term = reader.getTerm();
    MessageType type = reader.getMsgType();

    // Repeated fields
    auto entries = reader.getEntries();
    for (auto entry : entries) {
        uint64_t index = entry.getIndex();
        uint64_t term = entry.getTerm();
    }

    // Optional fields (check has*())
    if (reader.hasSnapshot()) {
        auto snapshot = reader.getSnapshot();
        auto data = snapshot.getData();
    }
}
```

### Writing Message Fields

```cpp
Message CreateMessage(uint64_t to, uint64_t from, const std::vector<Entry>& entries) {
    Message msg;
    auto builder = msg.builder();

    builder.setTo(to);
    builder.setFrom(from);
    builder.setTerm(current_term_);
    builder.setMsgType(MessageType::MSG_APPEND);

    auto entries_builder = builder.initEntries(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        entries_builder.setWithCaveats(i, entries[i].reader());
    }

    return msg;
}
```

### Converting Between Protobuf and Cap'n Proto (during transition)

If you need to support both temporarily:

```cpp
// Protobuf Entry -> Cap'n Proto Entry
Entry ConvertEntry(const ProtobufEntry& pb_entry) {
    Entry entry;
    auto builder = entry.builder();
    builder.setTerm(pb_entry.term());
    builder.setIndex(pb_entry.index());
    builder.setEntryType(ConvertEntryType(pb_entry.entry_type()));
    // ... convert other fields
    return entry;
}
```

---

## Performance Considerations

1. **Builder/Reader Caching**: Store builder/reader in local variable if used multiple times
   ```cpp
   // Good
   auto builder = msg.builder();
   builder.setTerm(5);
   builder.setFrom(1);
   builder.setTo(2);

   // Bad (multiple builder() calls)
   msg.builder().setTerm(5);
   msg.builder().setFrom(1);
   msg.builder().setTo(2);
   ```

2. **Serialization**: Cap'n Proto is generally faster than Protobuf, but avoid unnecessary serialization

3. **Zero-Copy**: Take advantage of Cap'n Proto's zero-copy design when possible

---

## Questions & Answers

**Q: Why can't I modify a message after reading it?**
A: Cap'n Proto separates builder (write) and reader (read) interfaces. Get a builder if you need to modify.

**Q: How do I clear a field?**
A: Set it to default value (0, empty string, etc.) or rebuild the message without that field.

**Q: Can I use Cap'n Proto messages across threads?**
A: Yes, but be careful with builder/reader lifetimes. Consider serializing/deserializing for thread boundaries.

**Q: What about backwards compatibility with old WAL files?**
A: This migration is breaking. Old WAL files using Protobuf won't work. Plan for snapshot-based recovery.

---

## Additional Resources

- [Cap'n Proto C++ Documentation](https://capnproto.org/cxx.html)
- [Cap'n Proto RPC Documentation](https://capnproto.org/cxxrpc.html)
- [Cap'n Proto Schema Language](https://capnproto.org/language.html)
- Project files:
  - `include/raftpp/core/types.h` - Type aliases and helpers
  - `include/raftpp/core/capnp_message.h` - OwnedMessage wrapper
  - `proto/raftpp.capnp` - Schema definition
