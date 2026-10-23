// The FPS limiter under Intel XeSS frame generation (fake libxess_fg.dll and libxell.dll, tests/flow/fakes/xefg).
// XeFG presents each game frame's outputs itself, the generated ones from its presenter thread, so CE paces the
// application frame on the game thread at xefgSwapChainSetPresentId (PresentSite::kRuntimeInputPresent) and lets
// XeFG's outputs pass. A game's own XeLL frame cap paces at xellSleep, in series with CE's: the lower cap must be
// the one that holds. The limiter waits on the virtual hook clock (CEFlow_EnableLimiterVirtualWaits) and so does the
// fake XeLL, so every cap is checked exactly: N rendered frames span N intervals of virtual time.

#include "tests/flow/flow_test_support.h"
#include "common/ipc/shared_defs.h"

namespace {

using namespace ce::flow;

constexpr int kXeFGStatusType = 4;  // ce::overlay_metrics::FGMetricType::kXeFG
constexpr int kOutputCapFps = 72;   // below the flow game's own 144 Hz frame clock, so every frame waits
constexpr int kMeasuredFrames = 120;

struct LimiterHooks {
    CEFlow_EnableLimiterVirtualWaits_t enableVirtualWaits = nullptr;
    CEFlow_GetLimiterStats_t stats = nullptr;
    CEFlow_ClockMicroseconds_t clockUs = nullptr;

    bool Resolve() {
        const HMODULE hook = GetModuleHandleW(L"capture_hook_x64.dll");
        enableVirtualWaits = reinterpret_cast<CEFlow_EnableLimiterVirtualWaits_t>(
            GetProcAddress(hook, "CEFlow_EnableLimiterVirtualWaits"));
        stats = reinterpret_cast<CEFlow_GetLimiterStats_t>(GetProcAddress(hook, "CEFlow_GetLimiterStats"));
        clockUs = reinterpret_cast<CEFlow_ClockMicroseconds_t>(GetProcAddress(hook, "CEFlow_ClockMicroseconds"));
        return enableVirtualWaits && stats && clockUs;
    }
    CEFlowLimiterStats Read() const {
        CEFlowLimiterStats out;
        stats(&out);
        return out;
    }
};

// Renders kMeasuredFrames and returns the virtual time they took.
int64_t TimedFrames(FlowGame& game, const LimiterHooks& hooks) {
    const int64_t start = hooks.clockUs();
    EXPECT_TRUE(game.RenderFrames(kMeasuredFrames)) << game.Error();
    return hooks.clockUs() - start;
}

void ExpectSpan(int64_t spanUs, int framesPerSecond, const char* phase, FlowGame& game) {
    const int64_t intervalUs = 1000000 / framesPerSecond;
    EXPECT_GE(spanUs, (kMeasuredFrames - 1) * intervalUs) << phase << " logs: " << game.LogDirectory();
    EXPECT_LE(spanUs, (kMeasuredFrames + 1) * intervalUs) << phase << " logs: " << game.LogDirectory();
}

// An XeFG game with CE's general cap at kOutputCapFps and the limiter on the virtual clock.
struct CappedXeFGGame {
    FlowGame game{CurrentTestName()};
    LimiterHooks hooks;
    SharedMemoryLayout* memory = nullptr;

    bool Start() {
        GameOptions options;
        options.swapchain = SwapchainKind::kXeFG;
        if (!game.CreateDeviceAndSwapchain(options) || !game.RenderFrames(60) || !hooks.Resolve())
            return false;
        hooks.enableVirtualWaits();
        memory = game.CaptureMemory();
        if (!memory)
            return false;
        memory->fpsLimiter.SetGeneralLimiterMode(0);  // basic
        memory->fpsLimiter.SetGeneralFps(kOutputCapFps);
        memory->fpsLimiter.SetGeneralEnabled(true);
        return true;
    }
    void Finish() {
        memory->fpsLimiter.SetGeneralEnabled(false);
        EXPECT_TRUE(game.RenderFrames(10)) << game.Error();
        ExpectEveryPresentCoveredOnce(game);
        ExpectNoDebugLayerErrors();
    }
};

void ExpectGameThreadPacesGroups(CappedXeFGGame& capped, uint32_t interpolatedFrames) {
    FlowGame& game = capped.game;
    const int multiplier = static_cast<int>(interpolatedFrames) + 1;
    ASSERT_TRUE(game.SetXeFGFrameGeneration(true, interpolatedFrames)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
    ExpectPublished(game, kXeFGStatusType, multiplier, "XeFG on");
    const uint64_t passesBefore = capped.hooks.Read().runtimeInputPasses;
    ExpectSpan(TimedFrames(game, capped.hooks), kOutputCapFps / multiplier, "XeFG on", game);
    const CEFlowLimiterStats stats = capped.hooks.Read();
    EXPECT_EQ(stats.resolvedMode, 0u);
    EXPECT_EQ(stats.cadenceTargetFps, kOutputCapFps);
    EXPECT_EQ(stats.cadenceScale, multiplier) << "the game thread must pace whole output groups";
    EXPECT_GE(stats.runtimeInputPasses - passesBefore, static_cast<uint64_t>(kMeasuredFrames) * multiplier)
        << "XeFG's outputs were paced by CE as well; logs: " << game.LogDirectory();
}

TEST(FlowXeFGLimiter, SetPresentIdPacesTheGameThreadAndOutputsPass) {
    CappedXeFGGame capped;
    ASSERT_TRUE(capped.Start()) << capped.game.Error();
    ExpectGameThreadPacesGroups(capped, 1);
    ExpectGameThreadPacesGroups(capped, 3);

    // Generation off: the proxy still carries the game's frames, one output each, at the configured rate.
    FlowGame& game = capped.game;
    ASSERT_TRUE(game.SetXeFGFrameGeneration(false, 1)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
    ExpectPublished(game, 0, 1, "XeFG off");
    ExpectSpan(TimedFrames(game, capped.hooks), kOutputCapFps, "XeFG off", game);
    const CEFlowLimiterStats stats = capped.hooks.Read();
    EXPECT_EQ(stats.cadenceTargetFps, kOutputCapFps);
    EXPECT_EQ(stats.cadenceScale, 1);
    capped.Finish();
}

// CE's 72 fps output cap at 2x is 36 rendered frames per second. The game's XeLL cap below that holds the frames
// itself and CE never adds a wait; above it, CE's cap holds and XeLL finds every frame late.
TEST(FlowXeFGLimiter, TheLowerOfCEsAndTheGamesXeLLCapPaces) {
    CappedXeFGGame capped;
    ASSERT_TRUE(capped.Start()) << capped.game.Error();
    FlowGame& game = capped.game;
    ASSERT_TRUE(game.SetXeFGFrameGeneration(true, 1)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
    ExpectPublished(game, kXeFGStatusType, 2, "XeFG on");

    constexpr int kXeLLLowerFps = 30;
    ASSERT_TRUE(game.SetXeLLFrameCap(1000000 / kXeLLLowerFps)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(30)) << game.Error();
    ExpectSpan(TimedFrames(game, capped.hooks), kXeLLLowerFps, "XeLL cap below CE's", game);

    ASSERT_TRUE(game.SetXeLLFrameCap(1000000 / 60)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(30)) << game.Error();
    ExpectSpan(TimedFrames(game, capped.hooks), kOutputCapFps / 2, "XeLL cap above CE's", game);

    ASSERT_TRUE(game.SetXeLLFrameCap(0)) << game.Error();
    capped.Finish();
}

}  // namespace
