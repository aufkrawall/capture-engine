// The FPS limiter under FSR frame generation (fake FidelityFX runtime, tests/flow/fakes/fidelityfx). AMD's
// swapchain presents each game frame's outputs from its own presenter thread and spaces them with its own pacer,
// so CE paces the game's Present into the proxy on the game thread (PresentSite::kRuntimeInputPresent) and lets
// the outputs pass. The limiter waits on the virtual hook clock (CEFlow_EnableLimiterVirtualWaits), so the cap is
// checked exactly: N rendered frames span N group intervals of virtual time, whatever WARP's real speed.

#include "tests/flow/flow_test_support.h"
#include "common/ipc/shared_defs.h"

namespace {

using namespace ce::flow;

constexpr int kOutputCapFps = 72;  // below the flow game's own 144 Hz frame clock, so every frame waits
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

void PaceFSRFrameGeneration(bool presentCallback) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.swapchain = SwapchainKind::kFidelityFX;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
    LimiterHooks hooks;
    ASSERT_TRUE(hooks.Resolve());
    hooks.enableVirtualWaits();
    SharedMemoryLayout* memory = game.CaptureMemory();
    ASSERT_NE(memory, nullptr);
    memory->fpsLimiter.SetGeneralLimiterMode(0);  // basic
    memory->fpsLimiter.SetGeneralFps(kOutputCapFps);
    memory->fpsLimiter.SetGeneralEnabled(true);

    // FG on: 72 displayed = 36 rendered groups of two, paced on the game thread; AMD's outputs pass.
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, presentCallback)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
    ExpectPublished(game, 2, 2, "FSR FG on");
    const uint64_t passesBefore = hooks.Read().runtimeInputPasses;
    ExpectSpan(TimedFrames(game, hooks), kOutputCapFps / 2, "FSR FG on", game);
    CEFlowLimiterStats stats = hooks.Read();
    EXPECT_EQ(stats.resolvedMode, 0u);
    EXPECT_EQ(stats.cadenceTargetFps, kOutputCapFps);
    EXPECT_EQ(stats.cadenceScale, 2) << "the game thread must pace whole output groups";
    EXPECT_GE(stats.runtimeInputPasses - passesBefore, static_cast<uint64_t>(kMeasuredFrames))
        << "AMD's outputs were paced by CE as well; logs: " << game.LogDirectory();

    // FG off: the proxy still carries the game's frames, one output each, at the configured rate.
    ASSERT_TRUE(game.SetFSRFrameGeneration(false, presentCallback)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
    ExpectPublished(game, 0, 1, "FSR FG off");
    ExpectSpan(TimedFrames(game, hooks), kOutputCapFps, "FSR FG off", game);
    stats = hooks.Read();
    EXPECT_EQ(stats.cadenceTargetFps, kOutputCapFps);
    EXPECT_EQ(stats.cadenceScale, 1);

    memory->fpsLimiter.SetGeneralEnabled(false);
    ASSERT_TRUE(game.RenderFrames(10)) << game.Error();
    ExpectNoDebugLayerErrors();
}

TEST(FlowFSRLimiter, PresentCallbackRoutePacesTheGameThreadAndPassesOutputs) {
    PaceFSRFrameGeneration(/*presentCallback=*/true);
}

TEST(FlowFSRLimiter, NoCallbackRoutePacesTheGameThreadAndPassesOutputs) {
    PaceFSRFrameGeneration(/*presentCallback=*/false);
}

}  // namespace
