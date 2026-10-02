#include <gtest/gtest.h>
#include <windows.h>
#include <timeapi.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>

#include "hook/fg/fg_detection.h"
#include "hook/pacing/fps_limiter_policy.h"
#include "hook/runtime/hook_common.h"
#include "hook/runtime/hook_context.h"
#include "hook/runtime/ipc_client.h"
#include "hook/pacing/reflex_limiter.h"

// Load dependencies globally, then compile the real inline limiter in an
// isolated namespace. Its unqualified clock/wait calls resolve to these fakes;
// production FpsLimiter and other suites keep the Windows clock. No real waits
// or scheduler-dependent assertions are needed to exercise bursts or deadlines.
namespace fps_limiter_burst_test {

inline int64_t g_nowUs = 1'000'000;

// Callable objects suppress argument-dependent lookup of the Windows APIs.
inline constexpr auto QueryPerformanceCounter = [](LARGE_INTEGER* counter) {
    counter->QuadPart = g_nowUs;
    g_nowUs += 10;
    return TRUE;
};

inline constexpr auto QueryPerformanceFrequency = [](LARGE_INTEGER* frequency) {
    frequency->QuadPart = 1'000'000;
    return TRUE;
};

inline constexpr auto CreateWaitableTimerExW = [](LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD) -> HANDLE {
    return nullptr;
};

inline constexpr auto Sleep = [](DWORD milliseconds) {
    g_nowUs += static_cast<int64_t>(milliseconds) * 1000;
};

inline constexpr auto SwitchToThread = []() {
    return TRUE;
};

inline constexpr auto timeBeginPeriod = [](UINT) -> MMRESULT {
    return TIMERR_NOERROR;
};

inline constexpr auto timeEndPeriod = [](UINT) -> MMRESULT {
    return TIMERR_NOERROR;
};

#include "hook/pacing/fps_limiter.h"

class FpsLimiterRuntimeOutputBurstTest : public ::testing::Test {
protected:
    std::unique_ptr<SharedMemoryLayout> shm;
    FpsLimiter limiter;

    void SetUp() override {
        g_nowUs = 1'000'000;
        shm = std::make_unique<SharedMemoryLayout>();
        limiter.SetSharedMemory(shm.get());
        shm->runtimeState.captureRequested = false;
        shm->runtimeState.isRecording = false;
        shm->fpsLimiter.SetGeneralEnabled(true);
        shm->fpsLimiter.SetGeneralFps(120);
        shm->fpsLimiter.SetGeneralLimiterMode(LimiterModeValues::kBasic);
        g_ReflexLimiter.SetGameActivated(false);
        g_FGCompat.SetDLSSFGActive(false);
        g_FGCompat.SetHeuristicFSRFGActive(false);
        g_FGCompat.SetDormantMode(true);
        g_FGCompat.SetFSRFGMultiplier(2);
        g_FGCompat.SetFSRFGActive(true);
    }

    void TearDown() override {
        shm->runtimeState.captureRequested = false;
        shm->fpsLimiter.SetGeneralEnabled(false);
        limiter.Apply();  // Release timer resolution and native handoff state.
        g_FGCompat.SetFSRFGActive(false);
        g_FGCompat.SetFSRFGMultiplier(0);
    }

    void ConfigureCaptureSync() {
        shm->runtimeState.captureRequested = true;
        shm->fpsLimiter.SetCaptureSyncEnabled(true);
        shm->fpsLimiter.SetCaptureSyncMultiplier(1);
        shm->fpsLimiter.SetCaptureFps(120);
        shm->fpsLimiter.SetCaptureSyncLimiterMode(LimiterModeValues::kBasic);
    }

    void ExpectEveryOutputPaced(int expectedFps) {
        constexpr auto site = ce::fps_limiter_policy::PresentSite::kRuntimeOutputPresent;
        for (uint32_t output = 1; output <= 4; ++output) {
            g_nowUs += 100;
            limiter.Apply(true, site);
            const auto resolved = limiter.GetResolvedCadence();
            EXPECT_EQ(resolved.generation, output) << "distinct runtime output skipped its cadence slot";
            EXPECT_EQ(resolved.targetFps, expectedFps);
            EXPECT_EQ(resolved.cadenceScale, 1);
        }
    }
};

TEST_F(FpsLimiterRuntimeOutputBurstTest, GeneralCapPacesEveryProvenOutputInABurst) {
    ExpectEveryOutputPaced(120);
}

TEST_F(FpsLimiterRuntimeOutputBurstTest, LowerGeneralCapPacesEveryOutputWhileRecording) {
    ConfigureCaptureSync();
    shm->fpsLimiter.SetGeneralFps(90);
    ExpectEveryOutputPaced(90);
}

TEST_F(FpsLimiterRuntimeOutputBurstTest, CaptureSyncPacesEveryProvenOutputInABurst) {
    ConfigureCaptureSync();
    ExpectEveryOutputPaced(120);
}

TEST_F(FpsLimiterRuntimeOutputBurstTest, LegacyDuplicateProneSiteStillDeduplicatesABurst) {
    constexpr auto site = ce::fps_limiter_policy::PresentSite::kDuplicateProne;
    limiter.Apply(true, site);
    const auto first = limiter.GetResolvedCadence();
    ASSERT_EQ(first.generation, 1u);
    ASSERT_EQ(first.targetFps, 60);
    g_nowUs += 100;
    limiter.Apply(true, site);
    EXPECT_EQ(limiter.GetResolvedCadence().generation, first.generation);
}

TEST_F(FpsLimiterRuntimeOutputBurstTest, NativeHandoffStillEvaluatesEveryProvenOutput) {
    struct NativeState {
        int targetCalls = 0;
        int sleepCalls = 0;
    } state;
    NativeFpsPacingBackend backend{};
    backend.context = &state;
    backend.isAvailable = [](void*) { return true; };
    backend.setTargetFps = [](void* context, int) {
        ++static_cast<NativeState*>(context)->targetCalls;
        return true;
    };
    backend.sleep = [](void* context, int64_t* waitUs) {
        ++static_cast<NativeState*>(context)->sleepCalls;
        *waitUs = 0;
        return true;
    };
    limiter.SetNativePacingBackend(backend);
    shm->fpsLimiter.SetGeneralLimiterMode(LimiterModeValues::kNative);
    constexpr auto site = ce::fps_limiter_policy::PresentSite::kRuntimeOutputPresent;
    for (int output = 1; output <= 4; ++output) {
        g_nowUs += 100;
        limiter.Apply(true, site);
        limiter.ApplyPostPresent();
        EXPECT_EQ(state.targetCalls, output);
        EXPECT_EQ(state.sleepCalls, output);
    }
    limiter.SetNativePacingBackend({});
}

class FpsLimiterFrontLoadVirtualClockTest : public FpsLimiterRuntimeOutputBurstTest {
protected:
    void SetUp() override {
        FpsLimiterRuntimeOutputBurstTest::SetUp();
        g_FGCompat.SetFSRFGActive(false);
        g_FGCompat.SetFSRFGMultiplier(0);
        shm->fpsLimiter.SetGeneralFps(240);
    }
};

// Strange Brigade DX12's 1.8 ms CPU frame can still miss the display deadline
// because of GPU work. A virtual clock keeps host scheduling from filling the
// reservation before this scenario supplies the GPU delay it is testing.
TEST_F(FpsLimiterFrontLoadVirtualClockTest, GpuWorkRunningPastTheDeadlineGrowsTheReservation) {
    constexpr auto site = ce::fps_limiter_policy::PresentSite::kUniqueApplicationPresent;
    limiter.SetObservedFrameWorkOverrideUs(1800);

    for (int i = 0; i < 96; ++i) {
        limiter.ObservePresentToDisplay(400);
        limiter.Apply(true, site);
        limiter.ApplyPostPresent();
    }
    const auto engaged = limiter.GetFrontLoadedPacingState();
    ASSERT_GT(engaged.releases, 0u);
    EXPECT_EQ(engaged.gpuHeadroomUs, 0);
    ASSERT_LT(engaged.budgetUs, engaged.intervalUs)
        << "the budget must still have room to grow, or the assertion below proves nothing";

    for (int i = 0; i < 192; ++i) {
        limiter.ObservePresentToDisplay(2400);
        limiter.Apply(true, site);
        limiter.ApplyPostPresent();
    }
    const auto grown = limiter.GetFrontLoadedPacingState();

    EXPECT_GT(grown.gpuHeadroomUs, 0) << "the reservation must cover the GPU half too";
    EXPECT_GT(grown.budgetUs, engaged.budgetUs);
    EXPECT_LE(grown.budgetUs, grown.intervalUs) << "it may saturate to the back edge, never past it";
}

}  // namespace fps_limiter_burst_test
