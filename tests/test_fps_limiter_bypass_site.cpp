#include "test_fps_limiter_shared.h"
#include "test_fps_limiter_sleep_mode_recorder.h"

#include <filesystem>
#include <string>

#include "source_fragment_reader.h"

// Talos 20261019_195220: after an FSR FG phase, Streamline presents every DLSS 4x output from its own thread
// through the post-FSR confirmed-standalone bypass. Applied as an application Present, the limiter evaluated two
// outputs per game frame; the second never saw a new Reflex Sleep, so the warmup after each FG-switch gap never
// reached the driver handoff and CE timer-paced Streamline's thread for 30 ms a frame (PC latency 115-145 ms).
namespace {

using ce::fps_limiter_policy::PresentSite;

TEST(FpsLimiterBypassSiteTest, PostFSRStandaloneBypassIsAFinalOutputBoundary) {
    using ce::fps_limiter_policy::ResolveBypassedFinalOutputSite;
    EXPECT_EQ(ResolveBypassedFinalOutputSite(false), PresentSite::kFinalOutputBoundary);
    EXPECT_EQ(ResolveBypassedFinalOutputSite(true), PresentSite::kRuntimeOutputPresent)
        << "a callback-proven FSR output keeps its own site";

    const std::string routing =
        ce::test_source::ReadFile(ce::test_source::FindSource("hook", "dxgi_shared_present_routing.cpp"));
    const size_t function = routing.find("void ApplyFpsLimiterBeforeBypassedFinalOutputPresent(");
    ASSERT_NE(function, std::string::npos);
    const size_t site = routing.find("ResolveBypassedFinalOutputSite(callbackProvenRuntimeOutput)", function);
    ASSERT_NE(site, std::string::npos);
    EXPECT_LT(site, routing.find("HRESULT ExecuteStartupRouting(", function));
}

class ReflexWarmupUnderMfgTest : public FpsLimiterTest {
protected:
    void SetUp() override {
        FpsLimiterTest::SetUp();
        UseVirtualClock();
        ResetManualRearmSetSleepModeRecorder();
        g_ReflexLimiter.TestInstallSetSleepModeForUnitTest(&TestManualRearmSetSleepMode,
                                                           reinterpret_cast<IUnknown*>(static_cast<uintptr_t>(0x6789)));
        mockShm->fpsLimiter.SetGeneralEnabled(true);
        mockShm->fpsLimiter.SetGeneralFps(120);
        // The session's mode: auto, which selected reflex.
        mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kAuto));
        g_FGCompat.SetDLSSFGMultiplier(4);
        g_FGCompat.SetDLSSFGActive(true);
        ConfirmDLSSFGPacing();
    }
    void TearDown() override {
        g_FGCompat.SetDLSSFGActive(false);
        g_ReflexLimiter.Shutdown();
        FpsLimiterTest::TearDown();
    }

    // One game frame at a 30 fps base: the game's Reflex Sleep, then four outputs 8.3 ms apart.
    void RunFrame(PresentSite site) {
        g_ReflexLimiter.MarkGameSleep("unit-test-frame");
        for (int output = 0; output < 4; ++output) {
            limiter.Apply(true, site);
            clock.Advance(TicksFromUs(8333));
        }
    }
};

TEST_F(ReflexWarmupUnderMfgTest, FinalOutputBoundaryHandsPacingToTheDriverAfterTheWarmup) {
    const uint32_t pacedBefore = limiter.GetPacedGroupCount();
    for (int frame = 0; frame < 6; ++frame) {
        RunFrame(PresentSite::kFinalOutputBoundary);
    }
    EXPECT_FALSE(limiter.IsActivelyLimiting()) << "the driver owns pacing; CE does not wait on the presenter";
    EXPECT_EQ(limiter.GetPacedGroupCount() - pacedBefore, 6u) << "one evaluated output per game frame";
    const int64_t frameTicks = VirtualTicksOf([&] { RunFrame(PresentSite::kFinalOutputBoundary); });
    EXPECT_EQ(frameTicks, TicksFromUs(8333) * 4) << "no CE wait once the warmup handed pacing to the driver";
}

TEST_F(ReflexWarmupUnderMfgTest, AnApplicationPresentSiteNeverFinishesTheWarmup) {
    for (int frame = 0; frame < 6; ++frame) {
        RunFrame(PresentSite::kUniqueApplicationPresent);
    }
    const int64_t frameTicks = VirtualTicksOf([&] { RunFrame(PresentSite::kUniqueApplicationPresent); });
    EXPECT_GT(frameTicks, TicksFromUs(8333) * 4)
        << "evaluating every output re-bases the fresh Sleep streak each frame: CE keeps timer-pacing";
}

}  // namespace
