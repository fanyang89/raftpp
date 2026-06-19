#pragma once

#include <chrono>
#include <functional>
#include <memory>

namespace raftpp::raftor {

class Runtime {
  public:
    using Clock = std::chrono::steady_clock;
    using LoopCallback = std::function<bool()>;
    using Callback = std::function<void()>;

    virtual ~Runtime() = default;

    virtual Clock::time_point Now() const = 0;
    virtual void Run(const LoopCallback& callback) = 0;
    virtual void Poll(std::chrono::milliseconds timeout, const Callback& callback) = 0;
    virtual void Wake() = 0;
};

using RuntimeFactory = std::function<std::unique_ptr<Runtime>()>;

std::unique_ptr<Runtime> MakeRuntime();
RuntimeFactory SetRuntimeFactoryForTesting(RuntimeFactory factory);

}  // namespace raftpp::raftor
