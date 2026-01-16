# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

raftpp is a C++ implementation of the RAFT consensus algorithm. It requires C++23 and uses CMake with Ninja as the build system.

## Build Commands

```bash
task cmake    # Configure CMake with dev preset
task build    # Build test targets
task test     # Build and run all tests
task pb       # Regenerate protobuf files from proto/raftpp.proto
task fmt      # Format code with clang-format (Linux only)
```

### Running Individual Tests

```bash
# Run specific test by name (doctest filter)
./build/tests/raftpp-tests "test_name"

# Run unit tests only
./build/tests/raftpp-tests

# Run data-driven tests only
./build/tests/datadriven/raftpp-datadriven-tests
```

### Build Options

Enable sanitizers via CMake:
```bash
cmake --preset=dev -DRAFTPP_SANITIZE=address
```

## Architecture

### Core Layers

1. **RawNode** (`include/raftpp/raw_node.h`) - User-facing API for integrating Raft into applications. Manages `Ready` structs that batch state changes for the application to process.

2. **Raft** (`include/raftpp/raft.h`) - Core consensus state machine extending `RaftCore`. Handles state transitions (Follower, Candidate, PreCandidate, Leader), message processing, and log replication coordination.

3. **RaftLog** (`include/raftpp/raft_log.h`) - Log management combining:
   - `Unstable` - In-memory buffer for uncommitted entries
   - `Storage` interface - Pluggable persistence backend

4. **ProgressTracker** (`include/raftpp/progress_tracker.h`) - Tracks replication state for peers, quorum calculations, and in-flight messages via `Inflights`.

5. **Configuration Management** - `ConfChanger`, `JointConf`, `MajorityConf` handle dynamic cluster membership with joint consensus.

### Key Patterns

**Error Handling**: Uses `std::expected<T, RaftError>` aliased as `Result<T>`:
```cpp
if (const auto result = operation(); !result) {
    return result.error();
}
```

**Message Dispatch**: Role-based message handling through `Step()` → `StepFollower()`, `StepCandidate()`, `StepLeader()`.

**Storage Interface**: Pure virtual interface in `storage.h`. `MemoryStorage` provides built-in implementation; custom backends can be implemented.

### Directory Structure

- `include/raftpp/` - Public headers
- `lib/` - Implementation files (.cc)
- `proto/` - Protobuf definitions and generated code
- `tests/` - Unit tests using doctest
- `tests/datadriven/` - Data-driven tests with text-based DSL (testdata/*.txt)
- `tests/harness/` - Test support infrastructure (network simulation, utilities)

### Entry Parameter Order

Entry constructors use `(index, term)` parameter order - be consistent with this convention.
