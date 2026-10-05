#include <gtest/gtest.h>

#include <latch>
#include <mutex>
#include <thread>

#include "hook/d3d12/frame_render_admission.h"

namespace {
bool AvailableToOtherThread(std::recursive_mutex& mutex) {
    bool available = false;
    std::thread competing([&] {
        available = mutex.try_lock();
        if (available) mutex.unlock();
    });
    competing.join();
    return available;
}
}

TEST(FrameRenderAdmissionTest, LockSpansDrawSubmissionCaptureAndEveryEarlyExit) {
    std::recursive_mutex mutex;
    for (int exitPhase = 0; exitPhase < 4; ++exitPhase) {
        [&] {
            ce::dx12::FrameRenderAdmission<std::recursive_mutex> admission;
            ASSERT_TRUE(admission.TryAcquire(mutex));
            for (int phase = 0; phase < 4; ++phase) {
                EXPECT_FALSE(AvailableToOtherThread(mutex));
                if (phase == exitPhase) return;
            }
        }();
        EXPECT_TRUE(AvailableToOtherThread(mutex));
    }
}

TEST(FrameRenderAdmissionTest, BusyAdmissionDoesNotBlockOrReleaseAnotherOwner) {
    std::recursive_mutex mutex;
    std::unique_lock held(mutex);
    bool admitted = true;
    std::thread competing([&] {
        ce::dx12::FrameRenderAdmission<std::recursive_mutex> admission;
        admitted = admission.TryAcquire(mutex);
    });
    competing.join();
    EXPECT_FALSE(admitted);
    EXPECT_FALSE(AvailableToOtherThread(mutex));
}

TEST(FrameRenderAdmissionTest, RetirementDrainsCallbackOutsideOverlayLockThenReacquires) {
    std::recursive_mutex overlay;
    std::mutex render;
    std::latch renderEntered{1};
    std::latch cancellationPublished{1};
    std::latch callbackDrained{1};
    std::thread callback([&] {
        std::lock_guard renderLock(render);
        renderEntered.count_down();
        cancellationPublished.wait();
        std::lock_guard overlayLock(overlay);
        callbackDrained.count_down();
    });
    renderEntered.wait();
    ce::dx12::FrameRenderAdmission<std::recursive_mutex> admission;
    ASSERT_TRUE(admission.TryAcquire(overlay));
    cancellationPublished.count_down();
    const int previousProof = admission.RetireOutsideLock([&] {
        std::lock_guard renderLock(render);
        callbackDrained.wait();
        EXPECT_TRUE(AvailableToOtherThread(overlay));
        return 7;
    });
    callback.join();
    EXPECT_EQ(previousProof, 7);
    EXPECT_FALSE(AvailableToOtherThread(overlay));
}

TEST(FrameRenderAdmissionTest, RoutedPresentEndsUnlockedAndNormalPresentReacquires) {
    std::recursive_mutex mutex;
    for (bool routed : {false, true}) {
        ce::dx12::FrameRenderAdmission<std::recursive_mutex> admission;
        ASSERT_TRUE(admission.TryAcquire(mutex));
        EXPECT_EQ(admission.RouteOutsideLock([&] {
            EXPECT_TRUE(AvailableToOtherThread(mutex));
            return routed;
        }), routed);
        EXPECT_EQ(AvailableToOtherThread(mutex), routed);
    }
}
