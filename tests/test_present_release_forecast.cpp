#include "test_fps_limiter_shared.h"

#include <cmath>
#include <optional>
#include <vector>

#include "common/capture/capture_policy/cfr_nearest_playout.h"
#include "hook/pacing/present_release_forecast.h"

// Pre-limiter capture stamping (hook/pacing/present_release_forecast.h). The DXGI detours copy the
// back buffer before the FPS limiter's pre-present wait, so a capture stamped with its own clock
// carried the game's build time instead of the grid the Present lands on: BioShock Infinite at a
// 120 fps capture-sync cap (20261019_164020) presented on an exact grid while its stamps moved
// ~4.5 ms with load, and media's phase lock answered with rephases, releases and repeat+skip pairs.
namespace {

namespace forecast = ce::present_release_forecast;

TEST(PresentReleaseForecastPolicyTest, ResolvesTheStampFromTheForecastLead) {
    const int64_t interval = 1000;
    const forecast::Forecast pending{50000, interval};

    const auto onGrid = forecast::ResolveSourceFrameStamp(49200, pending);
    EXPECT_EQ(onGrid.source, forecast::StampSource::kForecastRelease);
    EXPECT_EQ(onGrid.qpc, 50000) << "a capture inside the interval takes the pending release";

    const auto afterLateRecovery = forecast::ResolveSourceFrameStamp(50000 - 2 * interval, pending);
    EXPECT_EQ(afterLateRecovery.source, forecast::StampSource::kForecastRelease) << "two intervals is the bound";

    const auto stale = forecast::ResolveSourceFrameStamp(50000 - 2 * interval - 1, pending);
    EXPECT_EQ(stale.source, forecast::StampSource::kForecastOutOfRange);
    EXPECT_EQ(stale.qpc, 50000 - 2 * interval - 1);

    const auto passed = forecast::ResolveSourceFrameStamp(50000, pending);
    EXPECT_EQ(passed.source, forecast::StampSource::kDeadlinePassed) << "the limiter presents at once";
    EXPECT_EQ(passed.qpc, 50000);
    EXPECT_EQ(forecast::ResolveSourceFrameStamp(50400, pending).qpc, 50400);

    EXPECT_EQ(forecast::ResolveSourceFrameStamp(49200, {}).source, forecast::StampSource::kCaptureClock);
    EXPECT_EQ(forecast::ResolveSourceFrameStamp(49200, {50000, 0}).source, forecast::StampSource::kCaptureClock);
}

TEST(PresentReleaseForecastPolicyTest, AClaimMatchesOnlyItsOwnSlot) {
    EXPECT_TRUE(forecast::ClaimMatchesRelease(50000, 50000, 1000));
    EXPECT_TRUE(forecast::ClaimMatchesRelease(50000, 50250, 1000)) << "wake jitter";
    EXPECT_FALSE(forecast::ClaimMatchesRelease(50000, 50251, 1000));
    EXPECT_FALSE(forecast::ClaimMatchesRelease(50000, 49500, 1000)) << "a cadence re-based earlier";
    EXPECT_FALSE(forecast::ClaimMatchesRelease(50000, 50000, 0));
}

TEST(PresentReleaseForecastForecasterTest, ServesOnlyItsOwnerAndSettlesClaims) {
    forecast::Forecaster forecaster;
    forecaster.Publish(50000, 1000, 7);

    EXPECT_EQ(forecaster.StampCapture(49500, 8).source, forecast::StampSource::kCaptureClock)
        << "another thread's capture never takes this thread's deadline";
    const auto stamp = forecaster.StampCapture(49500, 7);
    EXPECT_EQ(stamp.source, forecast::StampSource::kForecastRelease);
    EXPECT_EQ(stamp.qpc, 50000);

    forecaster.SettleClaim(50020, 8);  // another thread's Apply() leaves the claim alone
    forecaster.SettleClaim(50020, 7);
    forecaster.ClearOwnedBy(8);
    EXPECT_EQ(forecaster.Read(7).releaseQpc, 50000) << "only the owner withdraws its forecast";
    forecaster.ClearOwnedBy(7);
    EXPECT_EQ(forecaster.Read(7).releaseQpc, 0);

    auto window = forecaster.TakeWindow();
    EXPECT_EQ(window.forecastStamps, 1u);
    EXPECT_EQ(window.captureClockStamps, 1u);
    EXPECT_EQ(window.claimsMatched, 1u);
    EXPECT_EQ(window.claimsMissed, 0u);
    EXPECT_EQ(window.leadQpcMax, 500);
    EXPECT_EQ(window.claimErrorQpcMax, 20);

    // A claim the next Apply() released on another slot, and one it released nothing for.
    forecaster.Publish(60000, 1000, 7);
    forecaster.StampCapture(59400, 7);
    forecaster.SettleClaim(60500, 7);
    forecaster.StampCapture(59400, 7);
    forecaster.SettleClaim(0, 7);
    window = forecaster.TakeWindow();
    EXPECT_EQ(window.claimsMissed, 1u);
    EXPECT_EQ(window.claimsUnreleased, 1u);
    EXPECT_EQ(forecaster.TakeWindow().forecastStamps, 0u) << "taking the window resets it";
}

struct PresentedFrame {
    int64_t captureQpc = 0;
    int64_t stampQpc = 0;
    int64_t releaseQpc = 0;
};

class PresentReleaseForecastLimiterTest : public FpsLimiterTest {
protected:
    void SetUp() override {
        FpsLimiterTest::SetUp();
        UseVirtualClock();
    }

    void EnableCaptureSync(int fps) {
        mockShm->runtimeState.captureRequested = true;
        mockShm->runtimeState.isRecording = true;
        mockShm->fpsLimiter.SetCaptureSyncEnabled(true);
        mockShm->fpsLimiter.SetCaptureSyncMultiplier(1);
        mockShm->fpsLimiter.SetCaptureFps(fps);
        mockShm->fpsLimiter.SetCaptureSyncLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));
    }

    // One DXGI present: the game builds the frame, ProcessFrame captures it, the limiter waits and
    // the Present goes out at the release.
    PresentedFrame Present(int64_t workTicks, bool preLimiterScope = true,
                           ce::fps_limiter_policy::PresentSite site = kUniquePresentSite) {
        clock.Advance(workTicks);
        PresentedFrame frame;
        std::optional<forecast::ScopedPreLimiterCapture> scope;
        if (preLimiterScope) {
            scope.emplace();
        }
        frame.captureQpc = clock.Now();
        frame.stampQpc = limiter.StampPreLimiterCapture(frame.captureQpc);
        limiter.Apply(true, site);
        frame.releaseQpc = clock.Now();
        return frame;
    }

    // Distance from the nearest slot of the exact 1/fps grid through `anchor`.
    double GridErrorTicks(int64_t qpc, int64_t anchor, int fps) const {
        const double interval = static_cast<double>(freq.QuadPart) / fps;
        const double slots = std::round(static_cast<double>(qpc - anchor) / interval);
        return std::abs(static_cast<double>(qpc - anchor) - slots * interval);
    }

    // The 20261019_164020 shape: build times that move with load (1.2-6.1 ms waits at 120 fps).
    static constexpr int64_t kWorkUs[] = {3000, 6500, 2400, 5100, 7000, 1900, 4400, 6800, 2600, 3900};
};

TEST_F(PresentReleaseForecastLimiterTest, CaptureSyncStampsEachFrameWithItsGridRelease) {
    EnableCaptureSync(120);
    const PresentedFrame first = Present(TicksFromUs(2000));
    EXPECT_EQ(first.stampQpc, first.captureQpc) << "nothing is forecast before the cadence exists";

    std::vector<PresentedFrame> frames;
    for (int frame = 0; frame < 40; ++frame) {
        frames.push_back(Present(TicksFromUs(kWorkUs[frame % 10])));
    }
    for (size_t i = 0; i < frames.size(); ++i) {
        EXPECT_EQ(frames[i].stampQpc, frames[i].releaseQpc) << "frame " << i << " carries its Present";
        EXPECT_GT(frames[i].stampQpc, frames[i].captureQpc) << "frame " << i << " was copied before the wait";
        EXPECT_LE(GridErrorTicks(frames[i].stampQpc, first.releaseQpc, 120), 1.0) << "frame " << i;
    }
    const auto window = limiter.TakeReleaseForecastWindowForTesting();
    EXPECT_EQ(window.forecastStamps, 40u);
    EXPECT_EQ(window.claimsMatched, 40u);
    EXPECT_EQ(window.claimsMissed + window.claimsUnreleased, 0u);
}

TEST_F(PresentReleaseForecastLimiterTest, AGeneralCapStampsItsGridTooAndUnscopedSitesKeepTheirClock) {
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(140);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));
    Present(TicksFromUs(2000));
    for (int frame = 0; frame < 10; ++frame) {
        const PresentedFrame presented = Present(TicksFromUs(kWorkUs[frame] / 2));
        EXPECT_EQ(presented.stampQpc, presented.releaseQpc) << "frame " << frame;
    }

    // A site that waits after its Present (D3D9, D3D8, OpenGL, DirectDraw) or captures after the wait
    // (Vulkan) never opens the scope: its clock already is the present instant, and its Apply()
    // withdraws the forecast so nothing stale outlives it.
    const PresentedFrame unscoped = Present(TicksFromUs(1000), /*preLimiterScope=*/false);
    EXPECT_EQ(unscoped.stampQpc, unscoped.captureQpc);
    EXPECT_EQ(limiter.PeekReleaseForecast().releaseQpc, 0);
    const PresentedFrame next = Present(TicksFromUs(1000));
    EXPECT_EQ(next.stampQpc, next.captureQpc) << "the unscoped Apply() published nothing to take";
}

TEST_F(PresentReleaseForecastLimiterTest, ALateFrameKeepsItsClockAndTheNextReturnsToTheGrid) {
    EnableCaptureSync(120);
    const PresentedFrame first = Present(TicksFromUs(2000));
    for (int frame = 0; frame < 5; ++frame) {
        Present(TicksFromUs(3000));
    }
    // A hitch: the frame takes 2.6 intervals, so the limiter finds its deadline passed.
    const PresentedFrame late = Present(TicksFromUs(21700));
    EXPECT_EQ(late.stampQpc, late.captureQpc);
    EXPECT_EQ(late.releaseQpc, late.captureQpc) << "presented immediately";

    // Capture sync skipped whole slots: the next stamp is back on the original grid phase.
    const PresentedFrame recovered = Present(TicksFromUs(1000));
    EXPECT_EQ(recovered.stampQpc, recovered.releaseQpc);
    EXPECT_LE(GridErrorTicks(recovered.stampQpc, first.releaseQpc, 120), 1.0);
    const auto window = limiter.TakeReleaseForecastWindowForTesting();
    EXPECT_EQ(window.deadlinePassed, 1u);
    EXPECT_EQ(window.claimsMissed, 0u);
}

TEST_F(PresentReleaseForecastLimiterTest, FrameGenerationAndAnInactiveLimiterWithdrawTheForecast) {
    EnableCaptureSync(120);
    Present(TicksFromUs(2000));
    Present(TicksFromUs(3000));
    ASSERT_NE(limiter.PeekReleaseForecast().releaseQpc, 0);

    // Generated outputs are not one Present per grid slot of this stream.
    g_FGCompat.SetDLSSFGMultiplier(2);
    g_FGCompat.SetDLSSFGActive(true);
    ConfirmDLSSFGPacing();
    Present(TicksFromUs(3000));
    EXPECT_EQ(limiter.PeekReleaseForecast().releaseQpc, 0);
    const PresentedFrame underFg = Present(TicksFromUs(3000));
    EXPECT_EQ(underFg.stampQpc, underFg.captureQpc);
    g_FGCompat.SetDLSSFGActive(false);
    g_ReflexLimiter.SetGameActivated(false);

    Present(TicksFromUs(3000));
    ASSERT_NE(limiter.PeekReleaseForecast().releaseQpc, 0);
    mockShm->runtimeState.captureRequested = false;
    mockShm->runtimeState.isRecording = false;
    Present(TicksFromUs(3000));
    EXPECT_EQ(limiter.PeekReleaseForecast().releaseQpc, 0) << "an inactive limiter holds no deadline";
}

TEST_F(PresentReleaseForecastLimiterTest, ACadenceThatRebasesAfterTheCaptureCountsAMissedClaim) {
    EnableCaptureSync(120);
    Present(TicksFromUs(2000));
    Present(TicksFromUs(3000));
    limiter.TakeReleaseForecastWindowForTesting();

    // Recording start re-targets the cadence between this frame's capture and its wait: the stamp
    // claimed the old grid's slot, the new cadence released half a new interval later.
    clock.Advance(TicksFromUs(3000));
    {
        forecast::ScopedPreLimiterCapture scope;
        const int64_t stamp = limiter.StampPreLimiterCapture(clock.Now());
        mockShm->fpsLimiter.SetCaptureFps(60);
        limiter.Apply(true, kUniquePresentSite);
        EXPECT_NE(stamp, clock.Now());
    }
    const auto window = limiter.TakeReleaseForecastWindowForTesting();
    EXPECT_EQ(window.claimsMissed, 1u) << "reported in the cadence stats line";
    EXPECT_GT(window.claimErrorQpcMax, 0);
}

// Media's CFR phase lock (cfr_nearest_playout.h) fed the two stamp shapes of the same 120 fps capture
// sync run: grid releases lock once and never move; capture-clock stamps whose build time steps
// with load move the observed phase past the lock's tolerance, which costs a rephase or release
// (each one a repeat+skip pair in the recording).
struct PhaseLockReplay {
    uint64_t acquisitions = 0;
    uint64_t rephases = 0;
    uint64_t releases = 0;
};

PhaseLockReplay ReplayPhaseLock(bool gridStamps) {
    namespace policy = ce::capture_policy;
    const int64_t frequency = 10000000;
    const int64_t interval = frequency / 120;
    const int64_t gridAnchor = 1000000000;
    const int64_t tickAnchor = gridAnchor + 2100;  // independent media grid phase
    const int64_t contentDelay = 300000;
    std::vector<int64_t> stamps;
    uint32_t jitter = 12345;
    for (int frame = 0; frame < 2400; ++frame) {
        const int64_t release = gridAnchor + frame * interval;
        jitter = jitter * 1103515245u + 12345u;
        const int64_t waitUs = ((frame / 600) % 2 == 0 ? 5300 : 1800) + static_cast<int64_t>(jitter % 600) - 300;
        stamps.push_back(gridStamps ? release : release - waitUs * 10);
    }
    policy::CfrCadencePhaseLockState state;
    size_t next = 0;
    for (int tick = 0; tick < 2300; ++tick) {
        const int64_t scheduled = tickAnchor + tick * interval + contentDelay;
        while (next < stamps.size() && stamps[next] <= scheduled) {
            policy::ObserveCfrCaptureSyncSourceTimestamp(state, stamps[next], interval);
            ++next;
        }
        if (next > 0) {
            policy::ApplyCfrCaptureSyncPhaseLock(state, scheduled - contentDelay, stamps[next - 1], interval, true);
        }
    }
    return {state.acquisitions, state.rephases, state.releases};
}

TEST(PresentReleaseForecastPhaseLockTest, GridReleasesLockOnceWhileLoadSteppedCaptureStampsMoveTheLock) {
    const PhaseLockReplay grid = ReplayPhaseLock(true);
    EXPECT_EQ(grid.acquisitions, 1u);
    EXPECT_EQ(grid.rephases, 0u);
    EXPECT_EQ(grid.releases, 0u);

    const PhaseLockReplay captureClock = ReplayPhaseLock(false);
    EXPECT_GT(captureClock.rephases + captureClock.releases, 0u);
}

}  // namespace
