# raftpp

A ready-to-use implementation of the [RAFT consensus algorithm][RAFT] written in C++.

## Build

```bash
task build
task test
```

## Raftor (high-level API)

`Raftor` is the ready-to-use orchestration layer on top of the core Raft state machine. It is designed to be driven by an external event loop.

Recommended integration pattern:

- Call `raftor->Start()` once.
- Drive progress by calling `raftor->Poll(0ms)` on every `tick_interval`.
- Threading contract: `Poll()` must be called from a single thread; transport callbacks are invoked on the `Poll()` thread.
- RPC transport runs its own internal `rpc_thread_`; you do not need to integrate KJ/libuv into your application's event loop.

Minimal example (timer-driven `Poll(0ms)`):

```cpp
#include <raftpp/raftor/raftor.h>

#include <chrono>
#include <memory>
#include <thread>

int main() {
  // Construct raftor + transport + storage using your application configuration.
  auto raftor = std::make_unique<raftpp::raftor::Raftor>(/* ... */);

  raftor->Start();

  const auto tick_interval = std::chrono::milliseconds{100};
  auto next = std::chrono::steady_clock::now();
  for (;;) {
    next += tick_interval;
    raftor->Poll(std::chrono::milliseconds{0});
    std::this_thread::sleep_until(next);
  }
}
```

[RAFT]: https://raft.github.io/
