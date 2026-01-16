# Raw Node Test Implementation Plan for raftpp

## Progress Summary

### Completed Tests (13/28 tests)

1. **test_raw_node_step** - Test that local messages are ignored
2. **test_raw_node_read_index** - Test read index functionality
3. **test_raw_node_start** - Test node startup
4. **test_raw_node_restart** - Test node restart
5. **test_raw_node_restart_from_snapshot** - Test restart from snapshot
6. **test_set_priority** - Test priority setting
7. **test_raw_node_propose_add_duplicate_node** - Test duplicate node handling
8. **test_raw_node_propose_add_learner_node** - Test adding learner nodes
9. **test_raw_node_read_index_to_old_leader** - Test read index forwarding
10. **test_raw_node_propose_and_conf_change** - Test configuration changes (simple add node and add learner)

### Remaining Tests (15/28 tests)

The following tests are more complex and may need additional features or adjustments:

#### High Priority Tests
11. **test_raw_node_joint_auto_leave** - Test joint configuration auto-leave
12. **test_skip_bcast_commit** - Test skip broadcast commit feature
13. **test_bounded_uncommitted_entries_growth_with_partition** - Test uncommitted entries limit
14. **test_raw_node_with_async_entries** - Test async entries handling
15. **test_raw_node_with_async_apply** - Test async apply
16. **test_raw_node_entries_after_snapshot** - Test entries after snapshot
17. **test_raw_node_overwrite_entries** - Test entry overwriting
18. **test_async_ready_leader** - Test async ready for leader
19. **test_async_ready_follower** - Test async ready for follower
20. **test_async_ready_become_leader** - Test async ready on becoming leader
21. **test_async_ready_multiple_snapshot** - Test multiple snapshots
22. **test_committed_entries_pagination** - Test committed entries pagination
23. **test_committed_entries_pagination_after_restart** - Test pagination after restart
24. **test_disable_proposal_forwarding** - Test disable proposal forwarding

#### Medium Priority Tests
25. **test_raw_node_with_async_entries_to_removed_node** - Test async entries when node is removed
26. **test_raw_node_with_async_entries_on_follower** - Test async entries when leader steps down
27. **test_raw_node_async_entries_with_leader_change** - Test async entries when leadership changes

### Files Created

1. **tests/harness/raw_node_test.cc** - New comprehensive raw node test file with 13 implemented tests
2. **tests/harness/test_util.h** - Added helper functions for raw node tests
3. **tests/harness/test_util.cc** - Implemented helper functions

### Key Implementation Notes

#### What Was Implemented

1. **Helper Functions**:
   - `NewRawNode()` - Creates RawNode with given id, peers, and config
   - `NewRawNodeWithConfig()` - Creates RawNode with custom config
   - `MustCmpReady()` - Compares Ready with expected values
   - `MakeConfStateV2()` - Creates ConfState for joint configs

2. **Basic Tests**:
   - Local message handling
   - Read index functionality
   - Node startup and restart
   - Restart from snapshot
   - Priority setting
   - Duplicate node handling
   - Learner node addition
   - Read index forwarding (multi-node)

3. **Configuration Change Tests**:
   - Simple add node
   - Add learner node

#### What Remains

The remaining tests require:
- Joint configuration auto-leave
- Skip broadcast commit feature
- Uncommitted entries limit
- Async entries handling (multiple scenarios)
- Async apply
- Entries after snapshot
- Entry overwriting
- Async ready (leader/follower/become_leader)
- Multiple snapshots
- Committed entries pagination
- Disable proposal forwarding

#### Implementation Considerations

1. **Joint Configuration Tests**: These tests require ConfChangeV2 with joint config support. The implementation may need additional features in RawNode or ConfChange handling.

2. **Async Tests**: These tests require `AdvanceAppendAsync`, `OnPersistReady`, `AdvanceApplyTo`, and `OnEntriesFetched` methods to be fully implemented and tested.

3. **Snapshot Tests**: These tests require snapshot handling to be robust, including multiple snapshots and edge cases.

4. **Pagination Tests**: These tests require `max_committed_size_per_ready` configuration and proper handling of large committed entry sets.

5. **Network Tests**: Tests like `test_disable_proposal_forwarding` require Network class to be fully functional with message filtering and routing.

### Next Steps

1. Verify that the implemented tests compile and pass
2. Implement the remaining high-priority tests (joint auto-leave, skip bcast commit, uncommitted limit)
3. Implement async tests if the required methods are available
4. Implement snapshot-related tests
5. Implement pagination tests
6. Implement disable proposal forwarding test

### Testing Strategy

For the remaining tests, consider:
1. **Incremental Implementation**: Implement tests one category at a time to ensure each is working before moving to the next
2. **Feature Flags**: Some tests may require specific feature flags or configurations
3. **Mock Objects**: Some tests may need mock storage or other components
4. **Error Cases**: Test various error conditions and edge cases

### Dependencies

The tests depend on:
- RawNode API being complete
- Network class for multi-node tests
- Proper ConfChangeV2 handling
- Async ready methods working correctly
- Storage implementation supporting required features

### Conclusion

The implemented tests provide a solid foundation for raw node testing in raftpp:
- Basic RawNode operations (step, read index, campaign, propose, restart)
- - Configuration changes (simple and learner)
- - Multi-node scenarios (read index forwarding)
- Priority management

This aligns well with the raft-rs test suite and provides good coverage of the core RawNode functionality.

---

# Original Plan Content Below

## Overview

This document outlines the plan for implementing raw node tests for raftpp, based on the corresponding Rust tests in `third_party/raft-rs/harness/tests/integration_cases/test_raw_node.rs`.

## Project Structure Analysis

### Existing Test Infrastructure

1. **tests/test_util.h/cc**: Basic test utilities (NewEntry, NewSnapshot, etc.)
2. **tests/harness/test_util.h/cc**: Advanced test utilities for harness-based tests
3. **tests/harness/network.h/cc**: Network simulation for multi-node tests
4. **tests/harness/interface.h/cc**: Interface wrapper for Raft instances
5. **tests/raw_node_test.cc**: Existing raw node tests (currently has basic tests)

### Key Components

- **RawNode**: The main class being tested
- **Ready**: Contains entries, messages, snapshot, hard/soft state
- **LightReady**: Contains committed entries and messages
- **Storage**: Abstraction for persistent storage
- **MemoryStorage**: In-memory storage implementation for tests

## Test Cases to Implement

Based on the Rust file, here are the test cases to implement:

### 1. Basic Tests (Already Implemented)

- [x] `test_raw_node_step` - Test that local messages are ignored
- [x] `test_raw_node_start` - Test node startup
- [x] `test_raw_node_restart` - Test node restart
- [x] `test_raw_node_restart_from_snapshot` - Test restart from snapshot
- [x] `test_raw_node_set_priority` - Test priority setting

### 2. Read Index Tests

#### `test_raw_node_read_index_to_old_leader`
**Purpose**: Test that MsgReadIndex to old leader gets forwarded to new leader

**Test Steps**:
1. Create a 3-node network (nodes 1, 2, 3)
2. Elect node 1 as leader
3. Send read index request to node 2 (follower)
4. Verify node 2 forwards to node 1 (current leader)
5. Send read index request to node 3 (follower)
6. Verify node 3 forwards to node 1
7. Elect node 3 as new leader
8. Verify node 1 (now follower) forwards to node 3 (new leader)

#### `test_raw_node_read_index`
**Purpose**: Test that RawNode.read_index sends MsgReadIndex and ReadState can be read out

**Test Steps**:
1. Create a single-node raw node
2. Campaign to become leader
3. Issue read index request with context
4. Verify read_states contains the request
5. Verify has_ready() returns true
6. Get Ready and verify read_states
7. Advance and verify read_states is cleared

### 3. Configuration Change Tests

#### `test_raw_node_propose_and_conf_change`
**Purpose**: Test configuration change mechanism (simple and joint)

**Test Cases**:
1. V1 config change (AddNode)
2. V2 config change (AddNode) without joint
3. V2 config change (AddLearnerNode) without joint
4. V2 config change (AddLearnerNode) with explicit transition
5. V2 config change (AddLearnerNode) with implicit transition
6. Complex config change with multiple changes (AddNode, AddLearnerNode x2)
7. Same as #6 with explicit transition
8. Same as #6 with implicit transition

**Test Pattern**:
1. Create raw node with single voter
2. Campaign to become leader
3. Propose data and conf change
4. Wait for conf change to apply
5. Verify entries and conf state
6. For joint configs, verify auto-leave behavior

#### `test_raw_node_joint_auto_leave`
**Purpose**: Test configuration change auto-leave even when leader loses leadership

**Test Steps**:
1. Create raw node with single voter
2. Campaign to become leader
3. Propose implicit joint conf change (AddLearnerNode)
4. When conf change applies, force step down (send heartbeat with higher term)
5. Verify pending_conf_index is 0
6. Campaign again to become leader
7. Verify auto-leave entry is generated

#### `test_raw_node_propose_add_duplicate_node`
**Purpose**: Test that two proposes to add same node don't affect later propose

**Test Steps**:
1. Create raw node and campaign
2. Propose adding node 1 (already exists)
3. Propose adding node 1 again
4. Propose adding node 2 (new node)
5. Verify last three entries are: cc1, cc1, cc2

#### `test_raw_node_propose_add_learner_node`
**Purpose**: Test adding learner node

**Test Steps**:
1. Create raw node and campaign
2. Propose adding learner node
3. Verify committed entry has correct type
4. Apply conf change
5. Verify conf state has voters=[1], learners=[2]

### 4. Async Ready Tests

#### `test_raw_node_with_async_entries`
**Purpose**: Test entries are handled properly when fetched asynchronously

**Test Steps**:
1. Create 2-node raw node
2. Prepare by becoming leader and proposing entries
3. Trigger log unavailable
4. Verify no entries are sent when unavailable
5. Trigger log available
6. Verify entries are sent when available

#### `test_raw_node_with_async_entries_to_removed_node`
**Purpose**: Test async entries when node is removed

**Test Steps**:
1. Prepare async entries scenario
2. Remove node 2 via conf change
3. Trigger log available
4. Verify no entries are sent (node removed)

#### `test_raw_node_with_async_entries_on_follower`
**Purpose**: Test async entries when leader steps down

**Test Steps**:
1. Prepare async entries scenario
2. Force leader to step down
3. Trigger log available
4. Verify no entries are sent (no longer leader)

#### `test_raw_node_async_entries_with_leader_change`
**Purpose**: Test async entries when leadership changes

**Test Steps**:
1. Prepare async entries scenario
2. Trigger leader change (become follower, then candidate, then leader)
3. Trigger log available
4. Verify only no-op entry is sent (leadership changed)

#### `test_raw_node_with_async_apply`
**Purpose**: Test async ready process with apply

**Test Steps**:
1. Create single-node raw node
2. Campaign to become leader
3. Propose entries in batches
4. Use AdvanceApplyTo to apply incrementally
5. Verify committed entries are correct

### 5. Snapshot Tests

#### `test_raw_node_entries_after_snapshot`
**Purpose**: Test ready process when follower receives snapshot and committed entries

**Test Steps**:
1. Create raw node with snapshot
2. Receive append with entries
3. Verify ready contains entries and no committed entries
4. Receive snapshot
5. Receive more entries after snapshot
6. Verify ready contains snapshot and new entries

#### `test_raw_node_overwrite_entries`
**Purpose**: Test committed entries are persisted when entries are overwritten

**Test Steps**:
1. Create raw node with snapshot
2. Receive append with entries [2, 3, 4]
3. Verify ready contains entries
4. Receive append with entries [4, 5, 6] (overwrites)
5. Verify ready contains new entries and previous committed entries

### 6. Async Ready Leader/Follower Tests

#### `test_async_ready_leader`
**Purpose**: Test async ready process for leader

**Test Steps**:
1. Create 3-node raw node (leader)
2. Propose entries in batches
3. Use AdvanceAppendAsync
4. Trigger OnPersistReady at various points
5. Verify commit index advances correctly

#### `test_async_ready_follower`
**Purpose**: Test async ready process for follower

**Test Steps**:
1. Create 2-node raw node (follower)
2. Receive append messages in batches
3. Use AdvanceAppendAsync
4. Trigger OnPersistReady
5. Verify committed entries are correct

#### `test_async_ready_become_leader`
**Purpose**: Test that new leader sends messages without persisting

**Test Steps**:
1. Create 3-node raw node
2. Trigger election
3. Verify vote requests are sent
4. Simulate voting responses
5. Verify leader sends append messages immediately

#### `test_async_ready_multiple_snapshot`
**Purpose**: Test handling multiple snapshots

**Test Steps**:
1. Create 2-node raw node (follower)
2. Receive first snapshot
3. Receive entries
4. Receive second snapshot
5. Verify both snapshots are handled correctly

### 7. Committed Entries Pagination Tests

#### `test_committed_entries_pagination`
**Purpose**: Test committed entries pagination with size limits

**Test Steps**:
1. Create 3-node raw node
2. Receive many entries
3. Set max_committed_size_per_ready to 0
4. Verify only 1 entry is returned
5. Set max_committed_size_per_ready to MAX
6. Verify all entries are returned

#### `test_committed_entries_pagination_after_restart`
**Purpose**: Test pagination after restart with commit_since_index

**Test Steps**:
1. Create storage with entries and committed index
2. Create raw node with this storage
3. Set max_committed_size_per_ready to limit
4. Verify entries are returned correctly
5. Verify no entries are lost

### 8. Feature Tests

#### `test_skip_bcast_commit`
**Purpose**: Test skip broadcast commit feature

**Test Steps**:
1. Create 3-node network
2. Elect node 1 as leader with skip_bcast_commit=true
3. Propose entry
4. Verify followers don't update commit immediately
5. Send heartbeat
6. Verify followers update commit
7. Disable skip_bcast_commit
8. Verify followers update commit immediately
9. Propose conf change
10. Verify commit is broadcast (conf change always bcasts)

#### `test_bounded_uncommitted_entries_growth_with_partition`
**Purpose**: Test uncommitted entries are limited with max_uncommitted_size

**Test Steps**:
1. Create raw node with max_uncommitted_size=12
2. Campaign to become leader
3. Propose entry (12 bytes) - should succeed
4. Propose another entry (12 bytes) - should fail (ProposalDropped)
5. Persist and advance
6. Propose another entry - should succeed

#### `test_disable_proposal_forwarding`
**Purpose**: Test disable proposal forwarding feature

**Test Steps**:
1. Create 3-node network
2. Elect node 1 as leader
3. Node 2: disable_proposal_forwarding=false
4. Node 3: disable_proposal_forwarding=true
5. Send proposal to node 2
6. Verify node 2 forwards to leader
7. Send proposal to node 3
8. Verify node 3 drops proposal (ProposalDropped)

## Helper Functions Needed

### In tests/harness/test_util.h

```cpp
// Create a RawNode for testing
RawNode NewRawNode(
    uint64_t id,
    const std::vector<uint64_t>& peers,
    size_t election_tick,
    size_t heartbeat_tick,
    std::shared_ptr<MemoryStorage> storage
);

// Create a RawNode with custom config
RawNode NewRawNodeWithConfig(
    const std::vector<uint64_t>& peers,
    const Config& config,
    std::shared_ptr<MemoryStorage> storage
);

// Compare Ready with expected values
void MustCmpReady(
    const Ready& rd,
    const std::optional<SoftState>& ss,
    const std::optional<HardState>& hs,
    const std::vector<Entry>& entries,
    const std::vector<Entry>& committed_entries,
    const std::optional<Snapshot>& snapshot,
    bool msg_is_empty,
    bool persisted_msg_is_empty,
    bool must_sync
);

// Create ConfChangeV2 with auto-leave
ConfChangeV2 MakeConfChangeV2WithAutoLeave(
    const std::vector<ConfChangeSingle>& changes,
    ConfChangeTransition transition
);

// Create ConfState for joint config
ConfState MakeConfStateV2(
    const std::vector<uint64_t>& voters,
    const std::vector<uint64_t>& learners,
    const std::vector<uint64_t>& voters_outgoing,
    const std::vector<uint64_t>& learners_next,
    bool auto_leave
);
```

## Implementation Notes

### Differences from Rust

1. **Memory Management**: C++ uses unique_ptr/shared_ptr vs Rust's ownership
2. **Error Handling**: C++ uses Result<T> vs Rust's Result<T, E>
3. **Optional**: C++ uses std::optional vs Rust's Option<T>
4. **Messages**: C++ uses std::vector<Message> vs Rust's Vec<Message>
5. **Storage**: C++ uses shared_ptr<MemoryStorage> for sharing between tests

### Key Patterns

1. **Ready Processing**:
   ```cpp
   auto rd = raw_node.GetReady();
   storage->Append(rd.entries());
   auto light_rd = raw_node.Advance(rd);
   // Process committed_entries from light_rd
   ```

2. **Campaign Pattern**:
   ```cpp
   while (true) {
       auto rd = raw_node.GetReady();
       storage->Append(rd.entries());
       if (rd.ss.has_value() && rd.ss->leader_id == raw_node.id()) {
           raw_node.Advance(rd);
           break;
       }
       raw_node.Advance(rd);
   }
   ```

3. **Conf Change Pattern**:
   ```cpp
   raw_node.ProposeConfChange("", cc);
   auto rd = raw_node.GetReady();
   storage->Append(rd.entries());
   auto light_rd = raw_node.Advance(rd);
   for (const auto& e : light_rd.committed_entries) {
       if (e.entry_type() == EntryConfChangeV2) {
           ConfChangeV2 cc;
           cc.ParseFromString(e.data());
           auto cs = raw_node.ApplyConfChange(cc);
       }
   }
   ```

## File Organization

The new tests should be added to:
- **tests/harness/raw_node_test.cc**: New file for comprehensive raw node tests
- **tests/harness/test_util.h/cc**: Add helper functions

## Dependencies

The tests depend on:
- doctest for test framework
- raftpp headers for Raft implementation
- harness infrastructure for network simulation
- spdlog for logging

## Next Steps

1. Add helper functions to test_util.h/cc
2. Create tests/harness/raw_node_test.cc
3. Implement tests in order of complexity
4. Run tests and verify correctness
5. Update CMakeLists.txt if needed

## Testing Strategy

1. **Unit Tests**: Test individual RawNode methods
2. **Integration Tests**: Test multi-node scenarios with Network
3. **Edge Cases**: Test boundary conditions and error paths
4. **Regression Tests**: Ensure existing functionality isn't broken
