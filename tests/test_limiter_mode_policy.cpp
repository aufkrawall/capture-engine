#include "test_fps_limiter_shared.h"

#include "hook/pacing/limiter_mode_policy.h"

// Limiter mode resolution (ce::limiter_mode_policy): which configured mode
// governs an Apply(), what auto picks from the runtime evidence, and how an
// unavailable native API is demoted. The pure matrix is exact; the fixture
// tests below prove Apply() routes through it for each constraint.

namespace {

using namespace ce::limiter_mode_policy;
constexpr uint32_t kBasicMode = LimiterModeValues::kBasic;
constexpr uint32_t kFGFallbackMode = LimiterModeValues::kFGFallback;
constexpr uint32_t kNativeMode = LimiterModeValues::kNative;
constexpr uint32_t kAutoMode = LimiterModeValues::kAuto;

}  // namespace

TEST(LimiterModePolicyTest, ConfiguredModeFollowsTheWinningConstraint) {
    EXPECT_EQ(ResolveConfiguredLimiterMode(true, false, kNativeMode, kBasicMode), kNativeMode);
    EXPECT_EQ(ResolveConfiguredLimiterMode(false, false, kNativeMode, kBasicMode), kBasicMode);
    // The vertical-blank ceiling is a bound, not a configured limiter.
    EXPECT_EQ(ResolveConfiguredLimiterMode(false, true, kBasicMode, kBasicMode), kAutoMode);
}

// Auto across {no FG, FG producing} x {game native pacing active or not}. The
// FG runtime kind is already folded into frameGenerationActive (DLSS-G needs a
// recent game Sleep to count as producing), so it is not a separate axis here.
TEST(LimiterModePolicyTest, AutoMatrix) {
    struct Case {
        bool nativeActive;
        bool fgActive;
        uint32_t expected;
    };
    const Case cases[] = {
        {false, false, kBasicMode},
        {false, true, kFGFallbackMode},
        {true, false, kNativeMode},
        {true, true, kNativeMode},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(ResolveAutoLimiterMode(c.nativeActive, c.fgActive), c.expected)
            << "native=" << c.nativeActive << " fg=" << c.fgActive;
        EXPECT_EQ(ResolveEffectiveLimiterMode(kAutoMode, c.nativeActive, c.fgActive), c.expected)
            << "native=" << c.nativeActive << " fg=" << c.fgActive;
    }
}

TEST(LimiterModePolicyTest, ExplicitModesIgnoreAutoEvidence) {
    for (const uint32_t mode : {kBasicMode, kFGFallbackMode, kNativeMode}) {
        for (const bool native : {false, true}) {
            for (const bool fg : {false, true}) {
                EXPECT_EQ(ResolveEffectiveLimiterMode(mode, native, fg), mode);
            }
        }
    }
}

TEST(LimiterModePolicyTest, UnavailableNativeApiDemotesToTheTimerPath) {
    EXPECT_EQ(ResolveUnavailableNativeLimiterMode(kNativeMode, true, false), kNativeMode);
    EXPECT_EQ(ResolveUnavailableNativeLimiterMode(kNativeMode, false, false), kBasicMode);
    EXPECT_EQ(ResolveUnavailableNativeLimiterMode(kNativeMode, false, true), kFGFallbackMode);
    EXPECT_EQ(ResolveUnavailableNativeLimiterMode(kBasicMode, false, true), kBasicMode);
}

TEST(LimiterModePolicyTest, ModeNames) {
    EXPECT_STREQ(LimiterModeName(kBasicMode), "basic");
    EXPECT_STREQ(LimiterModeName(kFGFallbackMode), "fg_fallback");
    EXPECT_STREQ(LimiterModeName(kNativeMode), "reflex");
    EXPECT_STREQ(LimiterModeName(kAutoMode), "auto");
    EXPECT_STREQ(LimiterModeName(99u), "basic");  // unknown values run the timer path
}

// Auto with FSR FG producing and no game Reflex resolves through the policy.
TEST_F(FpsLimiterTest, AutoResolvedModeWithFsrFrameGeneration) {
    UseVirtualClock();
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(120);
    mockShm->fpsLimiter.SetGeneralLimiterMode(kAutoMode);

    limiter.Apply(true, kUniquePresentSite);
    EXPECT_EQ(limiter.GetResolvedLimiterMode(), kBasicMode);

    g_FGCompat.SetFSRFGActive(true);
    limiter.Apply(true, kUniquePresentSite);
    EXPECT_EQ(limiter.GetResolvedLimiterMode(), ResolveAutoLimiterMode(false, true));
    g_FGCompat.SetFSRFGActive(false);
}

// Capture sync and the general limiter each carry their own mode; the
// constraint that wins target selection decides which one governs.
TEST_F(FpsLimiterTest, WinningConstraintSelectsWhichModeGoverns) {
    UseVirtualClock();
    mockShm->runtimeState.captureRequested = true;
    mockShm->runtimeState.isRecording = true;
    mockShm->fpsLimiter.SetCaptureSyncEnabled(true);
    mockShm->fpsLimiter.SetCaptureSyncMultiplier(1);
    mockShm->fpsLimiter.SetCaptureFps(60);
    mockShm->fpsLimiter.SetCaptureSyncLimiterMode(kFGFallbackMode);
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(144);
    mockShm->fpsLimiter.SetGeneralLimiterMode(kBasicMode);

    limiter.Apply(true, kUniquePresentSite);
    EXPECT_EQ(limiter.GetResolvedLimiterMode(), kFGFallbackMode);  // capture 60 < general 144

    mockShm->fpsLimiter.SetGeneralFps(30);
    limiter.Apply(true, kUniquePresentSite);
    EXPECT_EQ(limiter.GetResolvedLimiterMode(), kBasicMode);  // general 30 < capture 60
}
