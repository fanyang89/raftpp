# AGENTS.md

This file provides guidance for coding agents working with code in this repository.

## Project Overview

raftpp is a C++ implementation of the RAFT consensus algorithm. It currently builds with C++17 and uses CMake with Ninja as the build system.

## Language

- Chat communication: use Chinese (Simplified) when talking with the user/maintainer.
- Repo artifacts: use English for code, code comments, and documentation.

## Coding Style

This project follows the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html) with the following intentional deviation:

**Accessor Methods**: Simple getter methods that return member references use `snake_case` matching their member variable names (e.g., `raft_log()` for `raft_log_`). All other functions use `CamelCase` per Google style.

All code contributions must adhere to this style.

## Build Commands

```bash
task cmake    # Configure CMake with debug preset
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
cmake --preset=Debug -DRAFTPP_SANITIZE=address
cmake --preset=Debug -DRAFTPP_SANITIZE=thread
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

7. **Core Utilities** - `capnp_util.h` (Cap'n Proto helpers), `inflights.h` (in-flight tracking), `error.h`/`status.h` (error handling).

### High-Level Integration Components

1. **Raftor** (`raftor/`) - Complete orchestration layer managing the Raft lifecycle:
   - Single-threaded event loop model with ticking
   - Ready processing in correct order (`ready_processor.h`)
   - Thread-safe proposal/read APIs with callbacks, futures, and sync variants
   - Users implement `StateMachine` interface for application logic

2. **WAL** (`raftor/wal/`) - Write-Ahead Log for durable storage:
   - Segmented log files with CRC32C checksums
   - Metadata persistence for HardState/ConfState
   - Index for fast entry lookup
   - Log compaction via snapshots

3. **RPC Transport** (`raftor/rpc/`) - Network layer with pluggable transports:
   - `Transport` - Abstract interface for message passing
   - `CapnpTransport` - Cap'n Proto RPC implementation
   - `Codec` - Message encoding/decoding
   - `PeerManager` - Peer connection management

### Key Patterns

**Error Handling**: Uses `nonstd::expected<T, E>` aliased as `Result<T, E>`:

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
├── core/              # Core Raft implementation
│   ├── raft.h, raft_core.h, raft_config.h
│   ├── raw_node.h, raft_log.h, storage.h
│   ├── progress_tracker.h, progress.h, inflights.h
│   ├── conf_changer.h, joint_conf.h, majority_conf.h, tracker_conf.h
│   ├── read_only.h, error.h, status.h, types.h
│   └── ...
├── raftor/            # High-level orchestration
│   ├── raftor.h, raftor_config.h, state_machine.h
│   ├── proposal_tracker.h
│   ├── rpc/           # RPC transport layer
│   └── wal/           # Write-Ahead Log subsystem
└── ...

lib/                   # Implementation files (.cc), mirrors include structure
├── core/
├── raftor/
│   ├── raftor.cc, proposal_tracker.cc, ready_processor.h/.cc
│   ├── rpc/
│   └── wal/
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

### Logging and Output

- Do not write directly to the console from production code or tests: no `std::cout`, `std::cerr`, `std::clog`, `printf`/`fprintf`, `puts`, `fmt::print`, or `std::print`.
- Use `raftpp/logging.h` for logging. Prefer `RAFTPP_LOG_DEBUG/INFO/WARN/ERROR/CRITICAL`.
- `spdlog` is used as a backend dependency; avoid adding new direct `SPDLOG_*` or `spdlog::*` calls in normal project code unless you are changing the logging infrastructure itself.
- Quick check before shipping changes:
  - `rg -n --glob '!build/**' --glob '!.cache/**' "\\bstd::cout\\b|\\bstd::cerr\\b|\\bstd::clog\\b|\\bprintf\\s*\\(|\\bfprintf\\s*\\(\\s*stderr\\b|\\bfmt::print\\b|\\bstd::print\\b" .`

## MCP usage

Always use Context7 MCP when I need library/API documentation,
code generation, setup or configuration steps without me having to explicitly ask.

## Common Issues

### clangd doctest Errors

You may encounter clangd errors like:
```
[PasteError [23:1] Redefinition of 'DOCTEST_ANON_VAR_0'
:3076:1:
note: previous definition is here
```

This is a known clangd bug when analyzing doctest test files. These errors can be safely ignored - the code will build and run correctly.

## Dependencies

Agents can find CPM-downloaded dependency code under `.cache/cpm`.
