#include <gtest/gtest.h>

#include <atomic>
#include <latch>
#include <thread>
#include <vector>

#include "hook/d3d12/postsl_lifecycle.h"

using ce::dx12::PostSLLifecycle;

TEST(PostSLLifecycleTest, CallbackPublicationPrecedesAdmissionAndRemovalStopsNewWork) {
    PostSLLifecycle owner;
    owner.InstallCallback([&] { EXPECT_TRUE(owner.CallbacksEnabled()); });
    {
        PostSLLifecycle::Callback admitted(owner);
        EXPECT_TRUE(admitted);
        EXPECT_EQ(owner.CallbacksInFlight(), 1u);
        owner.RemoveCallback([&] { EXPECT_FALSE(owner.CallbacksEnabled()); });
        PostSLLifecycle::Callback rejected(owner);
        EXPECT_FALSE(rejected);
        EXPECT_EQ(owner.CallbacksInFlight(), 2u);
        // The admitted callback remains counted across cancellation.
    }
    EXPECT_EQ(owner.CallbacksInFlight(), 0u);
    owner.ResumeCallbacks();
    PostSLLifecycle::Callback resumed(owner);
    EXPECT_TRUE(resumed);
}

TEST(PostSLLifecycleTest, ConfirmationPublishesProofAndRejectsStaleEpochs) {
    PostSLLifecycle owner;
    const auto original = owner.Epoch();
    bool proof = false;
    EXPECT_TRUE(owner.ConfirmRender(original, [&] {
        EXPECT_FALSE(owner.ConfirmedInCurrentEpoch());
        proof = true;
    }));
    EXPECT_TRUE(proof);
    EXPECT_TRUE(owner.ConfirmedInCurrentEpoch());
    owner.BeginReactivation();
    EXPECT_FALSE(owner.ConfirmedInCurrentEpoch());
    EXPECT_TRUE(owner.ConfirmRender(original, [] {}));
    owner.InvalidateGeneration();
    EXPECT_FALSE(owner.ConfirmedInCurrentEpoch());
    proof = false;
    EXPECT_FALSE(owner.ConfirmRender(original, [&] { proof = true; }));
    EXPECT_FALSE(proof);
    EXPECT_TRUE(owner.ConfirmRender(owner.Epoch(), [] {}));
}

TEST(PostSLLifecycleTest, CancellationDuringProofCannotResurrectConfirmation) {
    PostSLLifecycle owner;
    EXPECT_FALSE(owner.ConfirmRender(owner.Epoch(), [&] { owner.InvalidateGeneration(); }));
    EXPECT_FALSE(owner.ConfirmedInCurrentEpoch());
}

TEST(PostSLLifecycleTest, RenderLockCoversRecordingAndSubmissionAndReleasesOnEarlyExit) {
    PostSLLifecycle owner;
    std::vector<int> phases;
    EXPECT_TRUE(owner.RenderTransaction([&](uint32_t epoch) {
        phases.push_back(1);  // entry
        for (int phase : {2, 3}) {  // recording and submission share admission
            bool competingRender = true;
            std::thread competing([&] { competingRender = owner.RenderTransaction([](uint32_t) {}); });
            competing.join();
            EXPECT_FALSE(competingRender);
            phases.push_back(phase);
        }
        EXPECT_TRUE(owner.ConfirmRender(epoch, [] {}));
        return;  // the same scope also owns every early return
    }));
    EXPECT_EQ(phases, (std::vector<int>{1, 2, 3}));
    EXPECT_TRUE(owner.RenderTransaction([](uint32_t) { return; }));
    EXPECT_TRUE(owner.RenderTransaction([](uint32_t) {}));
}

TEST(PostSLLifecycleTest, CallbackSpanningRetirementSeesCancellationBeforeRenderDrain) {
    PostSLLifecycle owner;
    owner.ResumeCallbacks();
    std::latch entered(1), cancelled(1);
    std::atomic<bool> retired{false};
    std::thread callback([&] {
        PostSLLifecycle::Callback admission(owner);
        EXPECT_TRUE(admission);
        EXPECT_TRUE(owner.RenderTransaction([&](uint32_t entryEpoch) {
            EXPECT_TRUE(owner.ConfirmRender(entryEpoch, [] {}));
            entered.count_down();
            cancelled.wait();
            EXPECT_EQ(owner.CallbacksInFlight(), 1u);
            EXPECT_FALSE(retired.load());
            EXPECT_NE(owner.Epoch(), entryEpoch);
            EXPECT_FALSE(owner.ConfirmedInCurrentEpoch());
            bool proof = false;
            EXPECT_FALSE(owner.ConfirmRender(entryEpoch, [&] { proof = true; }));
            EXPECT_FALSE(proof);
        }));
    });
    entered.wait();
    owner.PublishRetirement([] {});  // must precede the blocking render drain
    cancelled.count_down();
    EXPECT_EQ(owner.FinishRetirement([&] {
        retired.store(true);
        EXPECT_FALSE(owner.ConfirmedInCurrentEpoch());
        return 7;
    }), 7);
    callback.join();
    EXPECT_TRUE(retired.load());
    EXPECT_EQ(owner.CallbacksInFlight(), 0u);
    EXPECT_EQ(owner.FinishRetirement([] { return 8; }), 8);
}
