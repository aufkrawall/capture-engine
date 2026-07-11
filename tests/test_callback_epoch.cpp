#include <gtest/gtest.h>

#include <memory>

#include "../common/callback_epoch.h"

namespace {

struct CallbackOwner {
    int calls = 0;
};

TEST(CallbackEpochTest, StaleQueuedCallbackCannotEnterAfterStopOrRestart) {
    auto gate = ce::CallbackEpoch<CallbackOwner>::Create();
    CallbackOwner owner;

    const uint64_t firstEpoch = gate->Begin(&owner);
    {
        auto callback = gate->Enter(firstEpoch);
        ASSERT_TRUE(callback);
        ++callback->calls;
        EXPECT_EQ(gate->ActiveCount(), 1u);
    }
    EXPECT_EQ(owner.calls, 1);
    EXPECT_EQ(gate->ActiveCount(), 0u);

    gate->StopAndDrain();
    EXPECT_FALSE(gate->Enter(firstEpoch));

    const uint64_t secondEpoch = gate->Begin(&owner);
    EXPECT_NE(secondEpoch, firstEpoch);
    EXPECT_FALSE(gate->Enter(firstEpoch));
    auto secondCallback = gate->Enter(secondEpoch);
    EXPECT_TRUE(static_cast<bool>(secondCallback));
}

TEST(CallbackEpochTest, DetachedGateCanOutliveOwnerAndRejectLateDispatch) {
    auto gate = ce::CallbackEpoch<CallbackOwner>::Create();
    auto owner = std::make_unique<CallbackOwner>();
    const uint64_t epoch = gate->Begin(owner.get());

    gate->DetachAndDrain();
    owner.reset();

    // Models a WinRT handler that was queued before teardown but only began
    // after its owner had been destroyed. It touches shared gate state only.
    EXPECT_FALSE(gate->Enter(epoch));
    EXPECT_EQ(gate->ActiveCount(), 0u);
}

TEST(CallbackEpochTest, CallbackDrainIsPerInstance) {
    auto firstGate = ce::CallbackEpoch<CallbackOwner>::Create();
    auto secondGate = ce::CallbackEpoch<CallbackOwner>::Create();
    CallbackOwner firstOwner;
    CallbackOwner secondOwner;

    const uint64_t firstEpoch = firstGate->Begin(&firstOwner);
    secondGate->Begin(&secondOwner);
    auto activeFirstCallback = firstGate->Enter(firstEpoch);
    ASSERT_TRUE(activeFirstCallback);

    // A callback active on another capture instance must not delay this
    // instance's teardown (the old process-global counter did exactly that).
    secondGate->StopAndDrain();
    EXPECT_EQ(firstGate->ActiveCount(), 1u);

    activeFirstCallback.Reset();
    firstGate->StopAndDrain();
}

}  // namespace
