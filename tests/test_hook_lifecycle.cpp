#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>

#include "../hook/common/lifecycle.h"

namespace {

bool RunOneHookLifecycleCycle() {
    ce::HookLifecycle lifecycle;
    return lifecycle.TransitionTo(ce::HookState::Connecting) && lifecycle.TransitionTo(ce::HookState::Connected) &&
           lifecycle.TransitionTo(ce::HookState::Attaching) && lifecycle.TransitionTo(ce::HookState::Active) &&
           lifecycle.TransitionTo(ce::HookState::Detaching) && lifecycle.TransitionTo(ce::HookState::Disconnecting) &&
           lifecycle.TransitionTo(ce::HookState::Detached) && lifecycle.IsShuttingDown();
}

}  // namespace

TEST(HookLifecycleStressTest, RapidLifecycleChurnSingleThread) {
    constexpr int kCycles = 2000;
    for (int i = 0; i < kCycles; ++i) {
        ASSERT_TRUE(RunOneHookLifecycleCycle()) << "Cycle " << i << " failed";
    }
}

TEST(HookLifecycleStressTest, RapidLifecycleChurnMultiThread) {
    constexpr int kThreads = 6;
    constexpr int kCyclesPerThread = 400;
    std::atomic<int> failedCycles{0};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&failedCycles]() {
            for (int i = 0; i < kCyclesPerThread; ++i) {
                if (!RunOneHookLifecycleCycle()) {
                    failedCycles.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    EXPECT_EQ(failedCycles.load(std::memory_order_relaxed), 0);
}
