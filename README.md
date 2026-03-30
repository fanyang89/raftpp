# raftpp

A feature-complete implementation of the [RAFT consensus algorithm][RAFT] written in modern C++23.

## Features

- **Complete RAFT Implementation**: Leader election, log replication, membership changes, snapshots
- **Pre-vote Support**: Prevents disruption from partitioned nodes rejoining the cluster
- **Joint Consensus**: Safe cluster membership changes (add/remove multiple nodes)
- **Linearizable Reads**: Two modes - quorum-based (Safe) and lease-based (LeaseBased)
- **Pluggable Storage**: Abstract `Storage` interface with built-in WAL and in-memory implementations
- **Pluggable Transport**: Cap'n Proto RPC
- **Write-Ahead Log**: Segmented files, CRC32C checksums, io_uring support on Linux
- **OpenTelemetry Integration**: Built-in distributed tracing support

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      Your Application                       │
│                    (StateMachine impl)                      │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                         Raftor                              │
│              (High-level orchestration API)                 │
│         Propose / ReadIndex / AddNode / RemoveNode          │
└─────────────────────────────────────────────────────────────┘
         │                    │                    │
         ▼                    ▼                    ▼
┌─────────────┐      ┌─────────────┐      ┌─────────────┐
│  Transport  │      │   RawNode   │      │     WAL     │
│   (Cap'n)   │      │ (Raft Core) │      │  (Storage)  │
└─────────────┘      └─────────────┘      └─────────────┘
```

**Core Layer** (`include/raftpp/core/`): Pure RAFT state machine - `Raft`, `RawNode`, `RaftLog`, `ProgressTracker`, configuration management.

**Raftor Layer** (`include/raftpp/raftor/`): High-level orchestration with thread-safe APIs, timeout handling, and lifecycle management.

## Requirements

- C++23 compiler (GCC 13+, Clang 17+)
- CMake 3.20+
- Ninja (recommended)

## Build

```bash
task cmake    # Configure CMake with dev preset
task build    # Build all targets
task test     # Run all tests
task fmt      # Format code with clang-format
task check-fmt  # Verify formatting without modifying files
```

Formatting uses the repository Docker image end-to-end. Local `task fmt` and CI `Format Check`
both run `scripts/clang-format.sh` against the same containerized `clang-format` version.

### Build Options

```bash
# Enable sanitizers
cmake --preset=Debug -B build -DRAFTPP_SANITIZE=address

# Enable thread sanitizer
cmake --preset=Debug -B build -DRAFTPP_SANITIZE=thread

# Enable io_uring (Linux, requires system liburing via pkg-config)
cmake --preset=Debug -B build -DRAFTPP_WITH_LIBURING=ON
```

## Quick Start

### 1. Implement Your State Machine

```cpp
#include <array>

#include <raftpp/core/capnp_util.h>
#include <raftpp/raftor/state_machine.h>

class MyStateMachine final : public raftpp::raftor::StateMachine {
 public:
    raftpp::Result<raftpp::raftor::ApplyResult> Apply(const raftpp::Entry& entry) override {
        // Apply the committed entry to your application state
        // Return the result to be passed back to the proposer
        return raftpp::raftor::ApplyResult{"ok"};
    }

    raftpp::Result<raftpp::SnapshotMetadata> TakeSnapshot(
        uint64_t applied_index, uint64_t applied_term, const raftpp::ConfState& conf_state,
        raftpp::raftor::SnapshotWriter& writer
    ) override {
        const std::array<uint8_t, 1> payload = {'x'};
        if (auto result = writer.Write(payload); !result) {
            return nonstd::make_unexpected(result.error());
        }

        auto metadata = raftpp::capnp_util::make<raftpp::msg::SnapshotMetadata>();
        auto meta = raftpp::capnp_util::builder<raftpp::msg::SnapshotMetadata>(metadata);
        meta.setIndex(applied_index);
        meta.setTerm(applied_term);
        meta.setConfState(raftpp::capnp_util::reader<raftpp::msg::ConfState>(conf_state));
        return metadata;
    }

    raftpp::Result<void> RestoreSnapshot(
        const raftpp::SnapshotMetadata& metadata, raftpp::raftor::SnapshotReader& reader
    ) override {
        (void)metadata;
        std::array<uint8_t, 256> buffer{};
        while (true) {
            auto result = reader.Read(buffer);
            if (!result) {
                return nonstd::make_unexpected(result.error());
            }
            if (*result == 0) {
                return {};
            }
        }
    }
};
```

### 2. Configure and Start Raftor

```cpp
#include <chrono>

#include <raftpp/raftor/raftor.h>

using namespace std::chrono_literals;

int main() {
    raftpp::raftor::RaftorConfig config;
    config.node_id = 1;
    config.listen_addr = "127.0.0.1:9001";
    config.data_dir = "./minimal-node-data";
    config.tick_interval = 100ms;

    auto result = raftpp::raftor::Raftor::Create(config, std::make_unique<MyStateMachine>());
    if (!result) {
        return 1;
    }

    auto raftor = std::move(*result);

    if (auto result = raftor->Start(); !result) {
        return 1;
    }

    for (int i = 0; i < 20; ++i) {
        raftor->Poll(config.tick_interval);
        if (raftor->GetStatus().role == raftpp::StateRole::Leader) {
            raftor->Stop();
            return 0;
        }
    }

    raftor->Stop();
    return 1;
}
```

For a single-node bootstrap, leave `initial_peers` empty. Raftor will bootstrap the local node as the only voter.

### 3. Submit Proposals

```cpp
// Async with callback
raftor->Propose("my-data", [](raftpp::Result<std::string> result) {
    if (result) {
        std::cout << "Committed: " << *result << std::endl;
    }
});

// Sync (blocking)
auto result = raftor->ProposeSync("my-data");

// Async with future
auto future = raftor->ProposeAsync("my-data");
auto result = future.get();
```

### 4. Linearizable Reads

```cpp
// Request linearizable read confirmation
raftor->ReadIndex("read-ctx", [&](raftpp::Result<void> result) {
    if (result) {
        // Safe to read from state machine with linearizable consistency
        // Use raftor->GetStatus().applied_index to check progress
    }
});
```

## API Reference

### Raftor (High-Level API)

| Method | Description |
|--------|-------------|
| `Start()` | Start the Raft node |
| `Stop()` | Stop the Raft node |
| `Poll(timeout)` | Process pending events |
| `Propose(data, callback)` | Submit a proposal (async) |
| `ProposeSync(data)` | Submit a proposal (blocking) |
| `ProposeAsync(data)` | Submit a proposal (returns future) |
| `ReadIndex(ctx, callback)` | Request linearizable read index |
| `AddNode(id, addr)` | Add a new node to the cluster |
| `RemoveNode(id)` | Remove a node from the cluster |
| `TransferLeader(target_id)` | Transfer leadership |
| `GetStatus()` | Get current node status |
| `IsLeader()` | Check if this node is the leader |
| `TakeSnapshot()` | Trigger manual snapshot |

### RawNode (Low-Level API)

For advanced users who need fine-grained control:

```cpp
#include <raftpp/core/raw_node.h>

auto raw_node = raftpp::RawNode(config, storage);

// Main loop
while (raw_node.HasReady()) {
    auto ready = raw_node.GetReady();

    // 1. Save HardState and entries to stable storage
    // 2. Send messages to other nodes
    // 3. Apply committed entries to state machine
    // 4. Apply snapshot if any

    auto light_ready = raw_node.Advance(ready);
    // Process light_ready...
}
```

## Examples

See the `examples/` directory for complete examples:

- **minimal_node**: The smallest runnable Raftor example with a single `StateMachine` and manual `Poll()` loop
- **kvstore**: A distributed key-value store with HTTP REST API

## License

MIT License - see [LICENSE](LICENSE) for details.

[RAFT]: https://raft.github.io/
