# AGENTS.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

raftpp is a C++ implementation of the RAFT consensus algorithm. It requires C++23 and uses CMake with Ninja as the build system.

## Coding Style

This project follows the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html). All code contributions must adhere to this style.

## Build Commands

```bash
task cmake    # Configure CMake with dev preset
task build    # Build test targets
task test     # Build and run all tests
task fmt      # Format code with clang-format
```

### Running Individual Tests

```bash
# Run specific test by name (doctest filter)
./build/tests/raftpp-tests "test_name"

# Run unit tests only (alias: task ut)
./build/tests/raftpp-tests

# Run data-driven tests only (alias: task dt)
./build/tests/datadriven/data-driven-tests
```

### Build Options

Enable sanitizers via CMake:

```bash
cmake --preset=dev -DRAFTPP_SANITIZE=address
```

## Architecture

### Core Layers (`include/raftpp/core/`)

1. **RawNode** (`core/raw_node.h`) - User-facing API for integrating Raft into applications. Manages `Ready` structs that batch state changes for the application to process.

2. **Raft** (`core/raft.h`) - Core consensus state machine extending `RaftCore`. Handles state transitions (Follower, Candidate, PreCandidate, Leader), message processing, and log replication coordination.

3. **RaftLog** (`core/raft_log.h`) - Log management combining:
   - `Unstable` - In-memory buffer for uncommitted entries (`core/unstable_log.h`)
   - `Storage` interface - Pluggable persistence backend (`core/storage.h`)

4. **ProgressTracker** (`core/progress_tracker.h`) - Tracks replication state for peers, quorum calculations, and in-flight messages via `Inflights`.

5. **Configuration Management** - `ConfChanger`, `JointConf`, `MajorityConf`, `TrackerConf` handle dynamic cluster membership with joint consensus.

6. **ReadOnly** (`core/read_only.h`) - Read-only operation handling for linearizable reads.

7. **Core Utilities** - `capnp_message.h` (Cap'n Proto serialization), `inflights.h` (in-flight tracking), `error.h`/`status.h` (error handling).

### High-Level Integration Components

7. **Raftor** (`raftor/`) - Complete orchestration layer managing the Raft lifecycle:
   - Single-threaded event loop model with ticking
   - Ready processing in correct order (`ready_processor.h`)
   - Thread-safe proposal/read APIs with callbacks, futures, and sync variants
   - Users implement `StateMachine` interface for application logic

8. **WAL** (`raftor/wal/`) - Write-Ahead Log for durable storage:
   - Segmented log files with CRC32C checksums
   - Metadata persistence for HardState/ConfState
   - Index for fast entry lookup
   - Log compaction via snapshots

9. **RPC Transport** (`raftor/rpc/`) - Network layer with pluggable transports:
   - `Transport` - Abstract interface for message passing
   - `CapnpTransport` - Cap'n Proto RPC implementation
   - `RpclibTransport` - rpclib-based RPC implementation
   - `Codec` - Message encoding/decoding
   - `PeerManager` - Peer connection management

### Key Patterns

**Error Handling**: Uses `std::expected<T, RaftError>` aliased as `Result<T>`:

```cpp
if (const auto result = operation(); !result) {
    return result.error();
}
```

**Message Dispatch**: Role-based message handling through `Step()` → `StepFollower()`, `StepCandidate()`, `StepLeader()`.

**Storage Interface**: Pure virtual interface in `core/storage.h`. `MemoryStorage` provides built-in implementation; `WALStorage` provides persistence; custom backends can be implemented.

### Directory Structure

```
include/raftpp/
├── core/              # Core Raft implementation (25 headers)
│   ├── raft.h, raft_core.h, raft_config.h
│   ├── raw_node.h, raft_log.h, storage.h
│   ├── progress_tracker.h, progress.h, inflights.h
│   ├── conf_changer.h, joint_conf.h, majority_conf.h, tracker_conf.h
│   ├── read_only.h, error.h, status.h, types.h
│   └── ...
├── raftor/            # High-level orchestration (3 headers + subdirs)
│   ├── raftor.h, raftor_config.h, state_machine.h
│   ├── proposal_tracker.h
│   ├── rpc/           # RPC transport layer
│   └── wal/           # Write-Ahead Log subsystem
└── ...

lib/                   # Implementation files (.cc), mirrors include structure
├── core/              # 19 implementation files
├── raftor/
│   ├── raftor.cc, proposal_tracker.cc, ready_processor.h/.cc
│   ├── rpc/           # 4 transport implementations
│   └── wal/           # 9 WAL implementation files
└── ...

proto/
└── raftpp.capnp       # Cap'n Proto schema definitions

tests/
├── *.cc               # 15+ unit test files using doctest
├── datadriven/        # Data-driven tests with text-based DSL
│   ├── test_main.cc, datadriven.h/.cc
│   ├── confchange_test.cc, quorum_test.cc
│   └── testdata/confchange/, testdata/quorum/
└── harness/           # Test support infrastructure
```

### Entry Parameter Order

Entry constructors use `(index, term)` parameter order - be consistent with this convention.

### Using fmt Library

Always use spdlog's bundled fmt instead of the system fmt library:

```cpp
// Correct
#include <spdlog/fmt/fmt.h>

// Wrong - causes linker errors
#include <fmt/format.h>
```

## MCP usage

Always use Context7 MCP when I need library/API documentation,
code generation, setup or configuration steps without me having to explicitly ask.

## Dependencies

Agents can find CPM-downloaded dependency code under `.cache/cpm`.
