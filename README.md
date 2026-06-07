# raftpp

[![CI](https://github.com/fanyang89/raftpp/actions/workflows/ci.yml/badge.svg)](https://github.com/fanyang89/raftpp/actions/workflows/ci.yml)
[![codecov](https://codecov.io/github/fanyang89/raftpp/graph/badge.svg?token=1AWR0SLV3M)](https://codecov.io/github/fanyang89/raftpp)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![CMake](https://img.shields.io/badge/CMake-3.28%2B-blue.svg)](https://cmake.org)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![zread](https://img.shields.io/badge/Ask_Zread-_.svg?style=flat&color=00b0aa&labelColor=000000&logo=data%3Aimage%2Fsvg%2Bxml%3Bbase64%2CPHN2ZyB3aWR0aD0iMTYiIGhlaWdodD0iMTYiIHZpZXdCb3g9IjAgMCAxNiAxNiIgZmlsbD0ibm9uZSIgeG1sbnM9Imh0dHA6Ly93d3cudzMub3JnLzIwMDAvc3ZnIj4KPHBhdGggZD0iTTQuOTYxNTYgMS42MDAxSDIuMjQxNTZDMS44ODgxIDEuNjAwMSAxLjYwMTU2IDEuODg2NjQgMS42MDE1NiAyLjI0MDFWNC45NjAxQzEuNjAxNTYgNS4zMTM1NiAxLjg4ODEgNS42MDAxSDQuOTYxNTZDNS4zMTUwMiA1LjYwMDEgNS42MDE1NiA1LjMxMzU2IDUuNjAxNTYgNC45NjAxVjIuMjQwMUM1LjYwMTU2IDEuODg2NjQgNS4zMTUwMiAxLjYwMDEgNC45NjE1NiAxLjYwMDFaIiBmaWxsPSIjZmZmIi8%2BCjxwYXRoIGQ9Ik00Ljk2MTU2IDEwLjM5OTlIMi4yNDE1NkMxLjg4ODEgMTAuMzk5OSAxLjYwMTU2IDEwLjY4NjQgMS42MDE1NiAxMS4wMzk5VjEzLjc1OTlDMS42MDE1NiAxNC4xMTM0IDEuODg4MSAxNC4zOTk5IDIuMjQxNTYgMTQuMzk5OUg0Ljk2MTU2QzUuMzE1MDIgMTQuMzk5OSA1LjYwMTU2IDE0LjExMzQgNS42MDE1NiAxMy43NTk5VjExLjAzOTlDNS42MDE1NiAxMC42ODY0IDUuMzE1MDIgMTAuMzk5OSA0Ljk2MTU2IDEwLjM5OTlaIiBmaWxsPSIjZmZmIi8%2BCjxwYXRoIGQ9Ik0xMy43NTg0IDEuNjAwMUgxMS4wMzg0QzEwLjY4NSAxLjYwMDEgMTAuMzk4NCAxLjg4NjY0IDEwLjM5ODQgMi4yNDAxVjQuOTYwMUMxMC4zOTg0IDUuMzEzNTYgMTAuNjg1IDUuNjAwMSAxMS4wMzg0IDUuNjAwMUgxMy43NTg0QzE0LjExMTkgNS42MDAxIDE0LjM5ODQgNS4zMTM1NiAxNC4zOTg0IDQuOTYwMVYyLjI0MDFDMTQuMzk4NCAxLjg4NjY0IDE0LjExMTkgMS42MDAxIDEzLjc1ODQgMS42MDAxWiIgZmlsbD0iI2ZmZiIvPgo8cGF0aCBkPSJNNCAxMkwxMiA0TDQgMTJaIiBmaWxsPSIjZmZmIi8%2BCjxwYXRoIGQ9Ik00IDEyTDEyIDQiIHN0cm9rZT0iI2ZmZiIgc3Ryb2tlLXdpZHRoPSIxLjUiIHN0cm9rZS1saW5lY2FwPSJyb3VuZCIvPgo8L3N2Zz4K&logoColor=ffffff)](https://zread.ai/fanyang89/raftpp)

A modern C++ implementation of the [RAFT](https://raft.github.io/) consensus algorithm.

- **Batteries-included orchestration**: `Raftor` provides a single-threaded event loop, ready-processing, and thread-safe proposal / read APIs out of the box.
- **Production-grade persistence & networking**: Built-in segmented WAL with CRC32C and a pluggable Cap'n Proto RPC transport layer.
- **Extensively tested**: Unit tests, data-driven tests, and continuous ASan / TSan coverage on both x64 and ARM64.

## Features

- **Core consensus** — Full state machine with Follower, Candidate, PreCandidate, and Leader roles.
- **Log management** — `RaftLog` combines an in-memory unstable buffer with a pluggable `Storage` interface (`MemoryStorage`, `WALStorage`, or custom).
- **Dynamic membership** — Safe cluster configuration changes via joint consensus (`ConfChanger`, `JointConf`, `MajorityConf`).
- **Linearizable reads** — `ReadOnly` queue and read-index support for consistent read operations.
- **High-level orchestration** — `Raftor` manages the Raft lifecycle, ready processing, proposal tracking (callbacks, futures, sync variants), and snapshot flow.
- **Write-Ahead Log** — Segmented log files with CRC32C checksums, fast index lookup, and snapshot-based compaction.
- **Pluggable RPC** — Abstract `Transport` interface with a Cap'n Proto implementation (`CapnpTransport`).
- **Modern C++** — C++17, `nonstd::expected` error handling (`Result<T, E>`), and [doctest](https://github.com/doctest/doctest) test framework.

## Quick Start

### Prerequisites

- CMake >= 3.28
- Ninja
- C++17 compiler (Clang or GCC)
- [Task](https://taskfile.dev) (optional, for convenience commands)

### Build

Using Task:

```bash
task cmake    # configure
task build    # build tests
task test     # build & run all tests
```

Or with CMake directly:

```bash
cmake --preset=Debug -B build
cmake --build build
./build/tests/raftpp-tests
./build/tests/datadriven/data-driven-tests
```

### Minimal Example

```cpp
#include "raftpp/raftor/raftor.h"

class MyStateMachine : public raftpp::raftor::StateMachine {
 public:
  raftpp::Result<raftpp::raftor::ApplyResult> Apply(
      const raftpp::Entry& entry) override {
    return raftpp::raftor::ApplyResult{.response = "ok"};
  }
  // ... TakeSnapshot / RestoreSnapshot
};

int main() {
  raftpp::raftor::RaftorConfig config;
  config.node_id = 1;
  config.listen_addr = "127.0.0.1:9001";
  config.data_dir = "./data";
  config.tick_interval = std::chrono::milliseconds(100);

  auto raftor = raftpp::raftor::Raftor::Create(
      config, std::make_unique<MyStateMachine>());
  if (!raftor) { return 1; }

  if (auto result = (*raftor)->Start(); !result) { return 1; }

  // Drive the event loop...
  for (int i = 0; i < 20; ++i) {
    (*raftor)->Poll(config.tick_interval);
    if ((*raftor)->GetStatus().role == raftpp::StateRole::Leader) {
      (*raftor)->Stop();
      return 0;
    }
  }
  return 1;
}
```

See [`examples/minimal_node/`](examples/minimal_node) for the complete runnable version.

## Examples

| Example | Description |
|---------|-------------|
| [`examples/minimal_node/`](examples/minimal_node) | Single-node bootstrap that elects itself leader. |
| [`examples/kvstore/`](examples/kvstore) | Distributed key-value store with an HTTP frontend and multi-peer Raft cluster. |

## Architecture

```
┌─────────────────────────────────────────┐
│  Application (StateMachine, Proposals)  │
├─────────────────────────────────────────┤
│  Raftor  (event loop, ready processor,  │
│           proposal tracker)             │
├─────────────────────────────────────────┤
│  Core Raft  (RawNode → Raft → RaftLog)  │
├─────────────────────────────────────────┤
│  WAL + RPC Transport                    │
└─────────────────────────────────────────┘
```

- **Core** (`include/raftpp/core/`) — Low-level consensus primitives: `RawNode`, `Raft`, `RaftLog`, `Storage`, `ProgressTracker`, `ReadOnly`, and configuration changers.
- **Orchestration** (`raftor/`) — `Raftor` ties everything together with a tick-driven event loop, `Ready` processing, and user-friendly `Propose` / `ReadIndex` APIs.
- **I/O & Persistence** (`raftor/wal/`, `raftor/rpc/`) — Durable segmented WAL and a pluggable RPC layer (Cap'n Proto built-in).

## Testing

- **Unit tests** — `build/tests/raftpp-tests` (15+ test files, doctest-based).
- **Data-driven tests** — `build/tests/datadriven/data-driven-tests` (text-based DSL for quorum and confchange scenarios).
- **Sanitizers** — AddressSanitizer and ThreadSanitizer runs on every PR via GitHub Actions.
- **Coverage** — `llvm-cov` reports uploaded to Codecov.

## Documentation

Full documentation, guides, and API references are available at:

**https://raftpp.cc**

## License

MIT License. See [LICENSE](LICENSE).
