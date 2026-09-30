#include "test_fps_limiter_shared.h"

#include <filesystem>
#include <string>

#include "source_fragment_reader.h"

// PresentSite::kRuntimeOutputPresent: the DXGI Present of ONE frame-generation
// output, proven by the runtime's own present callback. Callback-owned native
// FSR FG presents every output (generated and application) from AMD's
// presenter thread, and since eec94472 inject capture records all of them.
//
// Talos, logs/20260930_032355 (capture sync 120, basic, FSR FG 2x):
//   Apply: ACTIVE ... target=120 effective=60 group=60/1 ... captureSource=final
//   Apply: ACTIVE ... target=120 effective=120 group=120/1 ... captureEq=240 captureSource=base
// Until media's inject handshake (~1.2 s) every output Present was paced at 60,
// halving the displayed rate at each recording start; afterwards the route was
// modelled as base capture, so capture sync compared as 240 fps against a
// concurrent general cap and drove a Reflex interval of 120 rendered frames.

namespace {

using ce::fps_limiter_policy::IsInjectCaptureFinalOutputForSite;
using ce::fps_limiter_policy::PresentSite;
using ce::fps_limiter_policy::ResolveDxgiPresentSite;
using ce::fps_limiter_policy::ResolveLocalCadencePlan;
using ce::fps_limiter_policy::ShouldGateEveryApplyOnCadenceGrid;
using ce::fps_limiter_policy::ShouldUseDuplicatePresentWindow;

inline constexpr PresentSite kRuntimeOutputSite = PresentSite::kRuntimeOutputPresent;

TEST(FpsLimiterRuntimeOutputSiteTest, CallbackProvenPresentSelectsTheRuntimeOutputSite) {
    EXPECT_EQ(ResolveDxgiPresentSite(true), PresentSite::kRuntimeOutputPresent);
    EXPECT_EQ(ResolveDxgiPresentSite(false), PresentSite::kUniqueApplicationPresent);
}

// The caller is the runtime's presenter thread: never the blocking strict grid.
TEST(FpsLimiterRuntimeOutputSiteTest, RuntimeOutputSiteNeverBlocksOnTheCadenceLock) {
    EXPECT_FALSE(ShouldGateEveryApplyOnCadenceGrid(kRuntimeOutputSite, true));
    EXPECT_FALSE(ShouldGateEveryApplyOnCadenceGrid(kRuntimeOutputSite, false));
}

TEST(FpsLimiterRuntimeOutputSiteTest, DuplicateWindowAndCadenceLockHaveSeparateContracts) {
    for (bool fgActive : {false, true}) {
        EXPECT_FALSE(ShouldUseDuplicatePresentWindow(kRuntimeOutputSite, fgActive));
        EXPECT_FALSE(ShouldUseDuplicatePresentWindow(PresentSite::kFinalOutputBoundary, fgActive));
        EXPECT_TRUE(ShouldUseDuplicatePresentWindow(PresentSite::kDuplicateProne, fgActive));
        EXPECT_EQ(ShouldUseDuplicatePresentWindow(PresentSite::kUniqueApplicationPresent, fgActive), fgActive);
    }
}

TEST(FpsLimiterRuntimeOutputSiteTest, RuntimeOutputSiteIsFinalOutputInjectCapture) {
    EXPECT_TRUE(IsInjectCaptureFinalOutputForSite(false, kRuntimeOutputSite));
    EXPECT_TRUE(IsInjectCaptureFinalOutputForSite(true, kRuntimeOutputSite));
    EXPECT_TRUE(IsInjectCaptureFinalOutputForSite(true, PresentSite::kUniqueApplicationPresent));
    EXPECT_FALSE(IsInjectCaptureFinalOutputForSite(false, PresentSite::kUniqueApplicationPresent));
    EXPECT_FALSE(IsInjectCaptureFinalOutputForSite(false, PresentSite::kDuplicateProne));
}

// Each entry is one output, so CE's own wait takes the output grid; a game-owned
// Reflex Sleep runs once per rendered frame and takes the exact group period.
TEST(FpsLimiterRuntimeOutputSiteTest, RuntimeOutputSitePacesEachOutputOnTheOutputGrid) {
    const auto plan = ResolveLocalCadencePlan(120, 60, true, 2, true, kRuntimeOutputSite);
    EXPECT_EQ(plan.presentTargetFps, 120);
    EXPECT_EQ(plan.presentCadenceTargetFps, 120);
    EXPECT_EQ(plan.presentCadenceScale, 1);
    EXPECT_EQ(plan.renderCadenceTargetFps, 120);
    EXPECT_EQ(plan.renderCadenceScale, 2);
}

TEST(FpsLimiterRuntimeOutputSiteTest, OtherSitesKeepTheirCadencePlans) {
    // Application Present under FG: base frames, as before.
    auto plan = ResolveLocalCadencePlan(120, 60, true, 2, true, PresentSite::kUniqueApplicationPresent);
    EXPECT_EQ(plan.presentTargetFps, 60);
    EXPECT_EQ(plan.presentCadenceTargetFps, 60);
    EXPECT_EQ(plan.presentCadenceScale, 1);
    EXPECT_EQ(plan.renderCadenceTargetFps, 60);
    EXPECT_EQ(plan.renderCadenceScale, 1);
    // Final-output boundary: whole groups on the exact rational grid.
    plan = ResolveLocalCadencePlan(130, 43, true, 3, true, PresentSite::kFinalOutputBoundary);
    EXPECT_EQ(plan.presentCadenceTargetFps, 130);
    EXPECT_EQ(plan.presentCadenceScale, 3);
    EXPECT_EQ(plan.renderCadenceTargetFps, 130);
    EXPECT_EQ(plan.renderCadenceScale, 3);
    // FG off: every site paces the configured rate unscaled.
    for (const PresentSite site : {PresentSite::kRuntimeOutputPresent, PresentSite::kUniqueApplicationPresent,
                                   PresentSite::kFinalOutputBoundary, PresentSite::kDuplicateProne}) {
        plan = ResolveLocalCadencePlan(120, 120, false, 1, true, site);
        EXPECT_EQ(plan.presentTargetFps, 120);
        EXPECT_EQ(plan.presentCadenceTargetFps, 120);
        EXPECT_EQ(plan.presentCadenceScale, 1);
        EXPECT_EQ(plan.renderCadenceScale, 1);
    }
}

class FpsLimiterRuntimeOutputApplyTest : public FpsLimiterTest {
protected:
    void ConfigureFsrCaptureSync120() {
        mockShm->runtimeState.captureRequested = true;
        mockShm->runtimeState.isRecording = true;
        mockShm->fpsLimiter.SetCaptureSyncEnabled(true);
        mockShm->fpsLimiter.SetCaptureSyncMultiplier(1);
        mockShm->fpsLimiter.SetCaptureFps(120);
        mockShm->fpsLimiter.SetCaptureSyncLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));
        g_FGCompat.SetFSRFGMultiplier(2);
        g_FGCompat.SetFSRFGActive(true);
    }

    void TearDown() override {
        mockShm->runtimeState.SetRuntimeFlag(kCaptureRuntimeFlagInjectVideoCaptureRequested, false);
        g_FGCompat.SetFSRFGActive(false);
        g_FGCompat.SetFSRFGMultiplier(0);
        FpsLimiterTest::TearDown();
    }
};

// The same 120 fps output grid before and after media's inject handshake, so a
// recording start no longer retargets the pacing of the displayed stream.
TEST_F(FpsLimiterRuntimeOutputApplyTest, CaptureSyncPacesEveryOutputAt120AcrossTheInjectHandshake) {
    ConfigureFsrCaptureSync120();

    limiter.Apply(true, kRuntimeOutputSite);
    auto resolved = limiter.GetResolvedCadence();
    ASSERT_GT(resolved.generation, 0u) << "Apply() did not reach the local cadence";
    EXPECT_EQ(resolved.targetFps, 120) << "pre-handshake capture sync must not halve the output";
    EXPECT_EQ(resolved.cadenceScale, 1);
    EXPECT_NEAR(resolved.intervalUs, 8333, 100);

    mockShm->runtimeState.SetRuntimeFlag(kCaptureRuntimeFlagInjectVideoCaptureRequested, true);
    const uint32_t generationBefore = resolved.generation;
    limiter.Apply(true, kRuntimeOutputSite);
    resolved = limiter.GetResolvedCadence();
    ASSERT_GT(resolved.generation, generationBefore);
    EXPECT_EQ(resolved.targetFps, 120);
    EXPECT_EQ(resolved.cadenceScale, 1);
    EXPECT_NEAR(resolved.intervalUs, 8333, 100);
}

// Capture sync now compares as 120 output fps: a 144 fps general cap must not
// win over it (it did while the route counted as 240 fps of base capture).
TEST_F(FpsLimiterRuntimeOutputApplyTest, CaptureSyncWinsAgainstAHigherGeneralCapInTheOutputDomain) {
    ConfigureFsrCaptureSync120();
    mockShm->runtimeState.SetRuntimeFlag(kCaptureRuntimeFlagInjectVideoCaptureRequested, true);
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(144);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));

    limiter.Apply(true, kRuntimeOutputSite);
    const auto resolved = limiter.GetResolvedCadence();
    ASSERT_GT(resolved.generation, 0u);
    EXPECT_EQ(resolved.targetFps, 120);
    EXPECT_EQ(resolved.cadenceScale, 1);

    mockShm->fpsLimiter.SetGeneralEnabled(false);
}

// A general cap alone is an output cap: each runtime output takes one slot of
// it, instead of the FG-divided base target that halved the displayed rate.
TEST_F(FpsLimiterRuntimeOutputApplyTest, GeneralCapPacesEachRuntimeOutputAtTheConfiguredRate) {
    mockShm->runtimeState.captureRequested = false;
    mockShm->runtimeState.isRecording = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(120);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));
    g_FGCompat.SetFSRFGMultiplier(2);
    g_FGCompat.SetFSRFGActive(true);

    limiter.Apply(true, kRuntimeOutputSite);
    const auto resolved = limiter.GetResolvedCadence();
    ASSERT_GT(resolved.generation, 0u);
    EXPECT_EQ(resolved.targetFps, 120);
    EXPECT_EQ(resolved.cadenceScale, 1);

    mockShm->fpsLimiter.SetGeneralEnabled(false);
}

// The application-Present site keeps its established base-frame pacing.
TEST_F(FpsLimiterRuntimeOutputApplyTest, ApplicationPresentSiteKeepsBaseCaptureSemantics) {
    ConfigureFsrCaptureSync120();
    mockShm->runtimeState.SetRuntimeFlag(kCaptureRuntimeFlagInjectVideoCaptureRequested, true);

    limiter.Apply(true, kUniquePresentSite);
    const auto resolved = limiter.GetResolvedCadence();
    ASSERT_GT(resolved.generation, 0u);
    EXPECT_EQ(resolved.targetFps, 120) << "base inject capture: the capture rate IS the rendered rate";
    EXPECT_EQ(resolved.cadenceScale, 1);
}

// Both DXGI limiter sites must choose their site from the verdict read at
// Present entry: ProcessFrame consumes it before the limiter runs, so a
// Consume or a late Peek at the Apply site would always read "unknown".
TEST(FpsLimiterRuntimeOutputSiteTest, DxgiLimiterSitesUseTheVerdictReadAtPresentEntry) {
    const auto read = [](const char* path) {
        return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / path);
    };
    const std::string present = read("hook/common/dxgi_shared_present.cpp");
    EXPECT_NE(present.find("ctx.callbackProvenRuntimeOutput = "
                           "ce::present_association::PeekPresentFrameVerdict().known;"),
              std::string::npos);
    const std::string core = read("hook/common/dxgi_shared_present_core.cpp");
    EXPECT_NE(core.find("ResolveDxgiPresentSite(ctx.callbackProvenRuntimeOutput)"), std::string::npos);

    const std::string present1 = read("hook/common/dxgi_shared_present1.cpp");
    const size_t entry = present1.find("NotePresentEntry(PerfLogger::GetQpcUs());");
    const size_t peek = present1.find("const bool callbackProvenRuntimeOutput = "
                                      "ce::present_association::PeekPresentFrameVerdict().known;");
    const size_t apply = present1.find("ResolveDxgiPresentSite(callbackProvenRuntimeOutput)");
    ASSERT_NE(entry, std::string::npos);
    ASSERT_NE(peek, std::string::npos);
    ASSERT_NE(apply, std::string::npos);
    EXPECT_LT(entry, peek);
    EXPECT_LT(peek, apply);
}

}  // namespace
