# raftpp

A feature-complete implementation of the [RAFT consensus algorithm][RAFT] written in modern C++23.

## Features

- **Complete RAFT Implementation**: Leader election, log replication, membership changes, snapshots
- **Pre-vote Support**: Prevents disruption from partitioned nodes rejoining the cluster
- **Joint Consensus**: Safe cluster membership changes (add/remove multiple nodes)
- **Linearizable Reads**: Two modes - quorum-based (Safe) and lease-based (LeaseBased)
- **Pluggable Storage**: Abstract `Storage` interface with built-in WAL and in-memory implementations
- **Pluggable Transport**: Cap'n Proto RPC (default) and optional RDMA for ultra-low-latency
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
│ (Cap'n/RDMA)│      │ (Raft Core) │      │  (Storage)  │
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
```

### Build Options

```bash
# Enable sanitizers
cmake --preset=dev -DRAFTPP_SANITIZE=address

# Enable RDMA transport (requires rdma-core)
cmake --preset=dev -DRAFTPP_WITH_RDMA=ON

# Enable io_uring (Linux)
cmake --preset=dev -DRAFTPP_WITH_LIBURING=ON
```

## Quick Start

### 1. Implement Your State Machine

```cpp
#include <raftpp/raftor/state_machine.h>

class MyStateMachine : public raftpp::raftor::StateMachine {
public:
    raftpp::raftor::ApplyResult Apply(raftpp::Entry entry) override {
        // Apply the committed entry to your application state
        // Return the result to be passed back to the proposer
        return {.data = "ok"};
    }

    raftpp::Result<raftpp::Snapshot> TakeSnapshot() override {
        // Serialize your application state for snapshots
    }

    raftpp::Result<void> RestoreSnapshot(raftpp::Snapshot snapshot) override {
        // Restore your application state from a snapshot
    }
};
```

### 2. Configure and Start Raftor

```cpp
#include <raftpp/raftor/raftor.h>

int main() {
    raftpp::raftor::RaftorConfig config;
    config.node_id = 1;
    config.listen_addr = "127.0.0.1:9001";
    config.initial_peers = {
        {1, "127.0.0.1:9001"},
        {2, "127.0.0.1:9002"},
        {3, "127.0.0.1:9003"},
    };
    config.data_dir = "/var/lib/myapp/raft";
    config.pre_vote = true;
    config.check_quorum = true;

    auto state_machine = std::make_shared<MyStateMachine>();
    auto raftor = raftpp::raftor::Raftor::Create(config, state_machine);

    raftor->Start();

    // Event loop
    const auto tick_interval = std::chrono::milliseconds{100};
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        next += tick_interval;
        raftor->Poll(std::chrono::milliseconds{0});
        std::this_thread::sleep_until(next);
    }
}
```

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
// Get read index, then read from state machine after index is applied
raftor->ReadIndex("read-ctx", [&](raftpp::Result<uint64_t> result) {
    if (result) {
        // Wait until applied_index >= *result, then read
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

## RDMA Transport

For ultra-low-latency clusters, enable RDMA transport:

```bash
cmake --preset=dev -DRAFTPP_WITH_RDMA=ON
```

Requires rdma-core user-space libraries (`libibverbs`, `librdmacm`).

```cpp
raftpp::raftor::RaftorConfig config;
config.transport_kind = raftpp::raftor::TransportKind::Rdma;
config.rdma.buffer_size = 1024 * 1024
    + raftpp::raftor::rpc::Codec::MessageOverhead()
    + raftpp::raftor::rpc::Codec::FrameOverhead();
```

Run RDMA tests:

```bash
RAFTPP_RDMA_TEST=1 \
RAFTPP_RDMA_ADDR1=10.0.0.1:19100 \
RAFTPP_RDMA_ADDR2=10.0.0.2:19101 \
task test
```

## Examples

See the `examples/` directory for complete examples:

- **kvstore**: A distributed key-value store with HTTP REST API

## License

MIT License - see [LICENSE](LICENSE) for details.

[RAFT]: https://raft.github.io/
