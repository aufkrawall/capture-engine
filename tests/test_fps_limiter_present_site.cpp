#include "test_fps_limiter_shared.h"

// Call-site present contracts (ce::fps_limiter_policy::PresentSite) and the
// cadence-grid gate they select. Kept separate from test_fps_limiter.cpp to
// stay under the source-size ceiling.

// Strange Brigade DX12 (session 20260913_122208) renders a new frame in
// 1-2ms, so a genuine next present repeatedly landed inside the 2ms
// duplicate-present window: 46 presents per second were classified as
// duplicates and reached the swapchain completely unpaced. The limiter's own
// stats stayed at a perfect "waited=120 late=0, avgFps=90.0" while the game
// presented ~130 fps against the 90 fps cap with alternating short/long frame
// times. A unique application-present site must therefore take a grid slot for
// an immediate second Apply instead of the dedup fast path.
TEST_F(FpsLimiterTest, UniqueApplicationPresentPacesImmediateSecondApply) {
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(60);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));

    limiter.Apply(true, kUniquePresentSite);

    bool sawFastDedup = false;
    bool sawPacedSecondApply = false;
    for (int attempt = 0; attempt < 3 && !sawPacedSecondApply; ++attempt) {
        LARGE_INTEGER start, end;
        QueryPerformanceCounter(&start);
        limiter.Apply(true, kUniquePresentSite);
        QueryPerformanceCounter(&end);

        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        const double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
        sawFastDedup = elapsedMs < 3.0 && limiter.GetLastWaitUs() == 0;
        sawPacedSecondApply = elapsedMs >= 3.0 && limiter.GetLastWaitUs() > 0;
    }

    EXPECT_FALSE(sawFastDedup);
    EXPECT_TRUE(sawPacedSecondApply);
}

// The unique-present contract only covers the application's own present
// stream. While a frame-generation runtime injects its generated presents into
// the same DXGI stream, CE still paces base frames there, so the established
// duplicate-window behaviour is retained rather than spending a base-rate grid
// slot on a present CE does not own.
TEST_F(FpsLimiterTest, UniqueApplicationPresentKeepsDuplicateWindowWhileFrameGenerationProduces) {
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(60);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));

    g_FGCompat.SetDLSSFGMultiplier(2);
    g_FGCompat.SetDLSSFGActive(true);
    ConfirmDLSSFGPacing();

    limiter.Apply(true, kUniquePresentSite);

    bool sawFastDedup = false;
    for (int attempt = 0; attempt < 3 && !sawFastDedup; ++attempt) {
        LARGE_INTEGER start, end;
        QueryPerformanceCounter(&start);
        limiter.Apply(true, kUniquePresentSite);
        QueryPerformanceCounter(&end);

        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        const double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
        sawFastDedup = elapsedMs < 3.0 && limiter.GetLastWaitUs() == 0;
    }

    g_FGCompat.SetDLSSFGActive(false);

    EXPECT_TRUE(sawFastDedup);
}

// A unique application-present site must not stall when the limiter is not
// configured, exactly like the final-output boundary.
TEST_F(FpsLimiterTest, UniqueApplicationPresentStaysNonBlockingWhenInactive) {
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(false);

    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    limiter.Apply(true, kUniquePresentSite);
    QueryPerformanceCounter(&end);

    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    const double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
    EXPECT_LT(elapsedMs, 100.0);
    EXPECT_FALSE(limiter.IsActivelyLimiting());
}

TEST(FpsLimiterPolicyTest, PresentSiteDecidesCadenceGateWithoutReadingAClock) {
    using ce::fps_limiter_policy::PresentSite;
    using ce::fps_limiter_policy::ShouldGateEveryApplyOnCadenceGrid;

    // Legacy duplicate-prone wrappers keep the duplicate window in both states.
    EXPECT_FALSE(ShouldGateEveryApplyOnCadenceGrid(PresentSite::kDuplicateProne, false));
    EXPECT_FALSE(ShouldGateEveryApplyOnCadenceGrid(PresentSite::kDuplicateProne, true));
    // A final-output boundary owns every presented output, FG included.
    EXPECT_TRUE(ShouldGateEveryApplyOnCadenceGrid(PresentSite::kFinalOutputBoundary, false));
    EXPECT_TRUE(ShouldGateEveryApplyOnCadenceGrid(PresentSite::kFinalOutputBoundary, true));
    // The DXGI application-present boundary owns its stream only while no
    // frame-generation runtime is presenting into it.
    EXPECT_TRUE(ShouldGateEveryApplyOnCadenceGrid(PresentSite::kUniqueApplicationPresent, false));
    EXPECT_FALSE(ShouldGateEveryApplyOnCadenceGrid(PresentSite::kUniqueApplicationPresent, true));
}
