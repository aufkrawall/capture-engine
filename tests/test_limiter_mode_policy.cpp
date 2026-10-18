#include "test_fps_limiter_shared.h"

#include "hook/pacing/limiter_mode_policy.h"

// Limiter mode resolution (ce::limiter_mode_policy): which configured mode
// governs an Apply(), what auto picks from the runtime evidence, and how an
// unavailable native API is demoted. The pure matrix is exact; the fixture
// tests below prove Apply() routes through it for each constraint.

namespace {

using namespace ce::limiter_mode_policy;
constexpr uint32_t kBasicMode = LimiterModeValues::kBasic;
constexpr uint32_t kNativeMode = LimiterModeValues::kNative;
constexpr uint32_t kAutoMode = LimiterModeValues::kAuto;
constexpr uint32_t kRetiredFGFallbackValue = 1;

// An API-native pacing backend (the Vulkan contract) whose game activity the
// test controls, so native selection does not depend on the host's nvapi64.
struct FakeNativeBackend {
    bool gameActive = false;
};

NativeFpsPacingBackend MakeFakeNativeBackend(FakeNativeBackend* fake) {
    NativeFpsPacingBackend backend{};
    backend.context = fake;
    backend.isAvailable = [](void*) { return true; };
    backend.isGameActive = [](void* context) { return static_cast<FakeNativeBackend*>(context)->gameActive; };
    backend.setTargetFps = [](void*, int) { return true; };
    backend.sleep = [](void*, int64_t* waitUs) {
        *waitUs = 0;
        return true;
    };
    backend.clear = [](void*) {};
    backend.name = "fake native backend";
    return backend;
}

}  // namespace

TEST(LimiterModePolicyTest, ConfiguredModeFollowsTheWinningConstraint) {
    EXPECT_EQ(ResolveConfiguredLimiterMode(true, false, kNativeMode, kBasicMode), kNativeMode);
    EXPECT_EQ(ResolveConfiguredLimiterMode(false, false, kNativeMode, kBasicMode), kBasicMode);
    // The vertical-blank ceiling is a bound, not a configured limiter.
    EXPECT_EQ(ResolveConfiguredLimiterMode(false, true, kBasicMode, kBasicMode), kAutoMode);
}

// Auto picks game-activated native pacing, otherwise basic. Frame generation
// is deliberately not an input: basic paces the displayed rate for every FG
// runtime, which is what the retired fg_fallback branch also did.
TEST(LimiterModePolicyTest, AutoMatrix) {
    EXPECT_EQ(ResolveAutoLimiterMode(false), kBasicMode);
    EXPECT_EQ(ResolveAutoLimiterMode(true), kNativeMode);
    EXPECT_EQ(ResolveEffectiveLimiterMode(kAutoMode, false), kBasicMode);
    EXPECT_EQ(ResolveEffectiveLimiterMode(kAutoMode, true), kNativeMode);
}

TEST(LimiterModePolicyTest, ExplicitModesIgnoreAutoEvidence) {
    for (const uint32_t mode : {kBasicMode, kNativeMode}) {
        for (const bool native : {false, true}) {
            EXPECT_EQ(ResolveEffectiveLimiterMode(mode, native), mode);
        }
    }
}

TEST(LimiterModePolicyTest, UnavailableNativeApiDemotesToBasic) {
    EXPECT_EQ(ResolveUnavailableNativeLimiterMode(kNativeMode, true), kNativeMode);
    EXPECT_EQ(ResolveUnavailableNativeLimiterMode(kNativeMode, false), kBasicMode);
    EXPECT_EQ(ResolveUnavailableNativeLimiterMode(kBasicMode, false), kBasicMode);
}

TEST(LimiterModePolicyTest, ModeNames) {
    EXPECT_STREQ(LimiterModeName(kBasicMode), "basic");
    EXPECT_STREQ(LimiterModeName(kNativeMode), "reflex");
    EXPECT_STREQ(LimiterModeName(kAutoMode), "auto");
    // Unknown values, the retired fg_fallback one included, run the timer path.
    EXPECT_STREQ(LimiterModeName(kRetiredFGFallbackValue), "basic");
    EXPECT_STREQ(LimiterModeName(99u), "basic");
}

TEST(LimiterModePolicyTest, RetiredFGFallbackParsesAsBasic) {
    EXPECT_EQ(ParseLimiterMode("fg_fallback"), LimiterMode::kBasic);
    EXPECT_EQ(ParseLimiterMode("fallback"), LimiterMode::kBasic);
    EXPECT_EQ(ParseLimiterMode("FG-Fallback"), LimiterMode::kBasic);
    EXPECT_EQ(static_cast<uint32_t>(LimiterMode::kNative), kNativeMode);
    EXPECT_EQ(static_cast<uint32_t>(LimiterMode::kAuto), kAutoMode);
}

// The FG x native matrix through Apply(): FSR FG and FG off resolve to basic
// without game-native pacing and to native with it.
TEST_F(FpsLimiterTest, AutoResolvedModeAcrossFrameGenerationAndNativeEvidence) {
    UseVirtualClock();
    FakeNativeBackend fake;
    limiter.SetNativePacingBackend(MakeFakeNativeBackend(&fake));
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(120);
    mockShm->fpsLimiter.SetGeneralLimiterMode(kAutoMode);

    // A frame of game work between presents: with FG producing, this site
    // keeps the duplicate-present window, which would swallow an Apply() that
    // arrives on the same virtual tick before it resolves any mode.
    const auto present = [&] {
        clock.Advance(TicksFromUs(10000));
        limiter.Apply(true, kUniquePresentSite);
    };
    for (const bool fsr : {false, true}) {
        g_FGCompat.SetFSRFGActive(fsr);
        fake.gameActive = false;
        present();
        EXPECT_EQ(limiter.GetResolvedLimiterMode(), kBasicMode) << "fsr=" << fsr;
        fake.gameActive = true;
        present();
        EXPECT_EQ(limiter.GetResolvedLimiterMode(), kNativeMode) << "fsr=" << fsr;
    }
    g_FGCompat.SetFSRFGActive(false);
    limiter.Shutdown();
}

// Capture sync and the general limiter each carry their own mode; the
// constraint that wins target selection decides which one governs.
TEST_F(FpsLimiterTest, WinningConstraintSelectsWhichModeGoverns) {
    UseVirtualClock();
    FakeNativeBackend fake;
    limiter.SetNativePacingBackend(MakeFakeNativeBackend(&fake));
    mockShm->runtimeState.captureRequested = true;
    mockShm->runtimeState.isRecording = true;
    mockShm->fpsLimiter.SetCaptureSyncEnabled(true);
    mockShm->fpsLimiter.SetCaptureSyncMultiplier(1);
    mockShm->fpsLimiter.SetCaptureFps(60);
    mockShm->fpsLimiter.SetCaptureSyncLimiterMode(kNativeMode);
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(144);
    mockShm->fpsLimiter.SetGeneralLimiterMode(kBasicMode);

    limiter.Apply(true, kUniquePresentSite);
    EXPECT_EQ(limiter.GetResolvedLimiterMode(), kNativeMode);  // capture 60 < general 144

    mockShm->fpsLimiter.SetGeneralFps(30);
    limiter.Apply(true, kUniquePresentSite);
    EXPECT_EQ(limiter.GetResolvedLimiterMode(), kBasicMode);  // general 30 < capture 60
    limiter.Shutdown();
}
