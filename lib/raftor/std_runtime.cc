#include <mutex>
#include <utility>

#include "runtime.h"

namespace raftpp::raftor {
namespace {

class StdRuntime final : public Runtime {
  public:
    Clock::time_point Now() const override { return Clock::now(); }

    void Run(const LoopCallback& callback) override {
        while (callback()) {}
    }

    void Poll(std::chrono::milliseconds /*timeout*/, const Callback& callback) override {
        callback();
    }

    void Wake() override {}
};

std::mutex& RuntimeFactoryMutex() {
    static std::mutex mutex;
    return mutex;
}

RuntimeFactory& TestRuntimeFactory() {
    static RuntimeFactory factory;
    return factory;
}

}  // namespace

std::unique_ptr<Runtime> MakeRuntime() {
    RuntimeFactory factory;
    {
        std::lock_guard lock(RuntimeFactoryMutex());
        factory = TestRuntimeFactory();
    }
    if (factory) {
        return factory();
    }
    return std::make_unique<StdRuntime>();
}

RuntimeFactory SetRuntimeFactoryForTesting(RuntimeFactory factory) {
    std::lock_guard lock(RuntimeFactoryMutex());
    auto previous = std::move(TestRuntimeFactory());
    TestRuntimeFactory() = std::move(factory);
    return previous;
}

}  // namespace raftpp::raftor
