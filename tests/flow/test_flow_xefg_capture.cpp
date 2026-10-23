// Recording an Intel XeSS frame generation game with inject capture (fake libxess_fg.dll, tests/flow/fakes/xefg).
// XeFG presents every output itself, the generated ones from its presenter thread on its own queue. Capture must
// record those outputs as presented - the generated ones included, so at the output rate - with or without the
// overlay, and keep recording through generation toggles, a factor change and the game leaving XeFG for its own
// swapchain and coming back, each swapchain publishing a transport generation of its own. Each output carries its
// own presentation time, generated or not, so the recording's constant frame rate is built from evenly spaced
// frames: at 2x, two outputs per 144 Hz game frame, one every 3.47 ms of the scenario's virtual clock.

#include "tests/flow/flow_capture_reader.h"
#include "tests/flow/flow_test_support.h"
#include "common/ipc/shared_defs.h"

#include <algorithm>

namespace {

using namespace ce::flow;

constexpr int kXeFGStatusType = 4;  // ce::overlay_metrics::FGMetricType::kXeFG

// Frames the consumer must have read for `gameFrames` game frames at `outputsPerFrame`: every output, less one
// group either side for the frames in flight when the phase starts and ends.
uint64_t AllOutputs(int gameFrames, int outputsPerFrame) {
    return static_cast<uint64_t>(gameFrames - 1) * static_cast<uint64_t>(outputsPerFrame);
}

// The recorded outputs from `first` on are spaced one output interval apart (half an interval of slack).
void ExpectEvenlySpaced(const CaptureReader& reader, size_t first, int outputsPerFrame, FlowGame& game) {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const double expectedUs = 1000000.0 / 144.0 / outputsPerFrame;
    double minimumUs = 1e12;
    double maximumUs = 0.0;
    for (size_t i = first + 1; i < reader.timeline.size(); ++i) {
        const double us = static_cast<double>(reader.timeline[i] - reader.timeline[i - 1]) * 1e6 /
                          static_cast<double>(frequency.QuadPart);
        minimumUs = (std::min)(minimumUs, us);
        maximumUs = (std::max)(maximumUs, us);
    }
    ASSERT_GT(reader.timeline.size(), first + 8);
    EXPECT_GE(minimumUs, expectedUs * 0.5) << "two outputs stamped together; logs: " << game.LogDirectory();
    EXPECT_LE(maximumUs, expectedUs * 1.5) << "an output's time is missing; logs: " << game.LogDirectory();
}

void RecordXeFG(bool includeOverlay) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.swapchain = SwapchainKind::kXeFG;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ASSERT_TRUE(game.SetXeFGFrameGeneration(true, 1)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ExpectPublished(game, kXeFGStatusType, 2, "XeFG on before recording");
    auto* memory = game.CaptureMemory();
    ASSERT_NE(memory, nullptr);
    memory->BeginWriteOverlayConfig();
    memory->overlayConfig.captureIncludeOverlay = includeOverlay;
    memory->EndWriteOverlayConfig();
    const auto capture = reinterpret_cast<void (*)(bool)>(GetProcAddress(
        GetModuleHandleW(L"capture_hook_x64.dll"), "CEFlow_SetInjectCapture"));
    ASSERT_NE(capture, nullptr);
    capture(true);
    memory->fpsLimiter.SetCaptureFps(0);  // observe every output, independently of the CFR cadence gate
    CaptureReader reader(game);
    auto record = [&](int count) {
        for (int i = 0; i < count; ++i) {
            if (!game.RenderFrame() || !reader.Consume())
                return false;
        }
        return true;
    };

    ASSERT_TRUE(record(60)) << game.Error();
    EXPECT_GE(reader.frames, AllOutputs(60, 2)) << "capture must record XeFG's generated outputs; logs: "
                                                << game.LogDirectory();
    ExpectEvenlySpaced(reader, 2, 2, game);
    uint64_t before = reader.frames;

    ASSERT_TRUE(game.SetXeFGFrameGeneration(true, 3)) << game.Error();
    ASSERT_TRUE(record(2)) << game.Error();
    const size_t fourTimesFirst = reader.timeline.size();
    ASSERT_TRUE(record(58)) << game.Error();
    EXPECT_GE(reader.frames - before, AllOutputs(60, 4)) << "4x: every output; logs: " << game.LogDirectory();
    ExpectEvenlySpaced(reader, fourTimesFirst, 4, game);
    before = reader.frames;

    ASSERT_TRUE(game.SetXeFGFrameGeneration(false, 1)) << game.Error();
    ASSERT_TRUE(record(40)) << game.Error();
    EXPECT_GE(reader.frames - before, AllOutputs(40, 1)) << "generation off keeps recording";
    before = reader.frames;

    // The game leaves XeFG for its own swapchain and comes back with a new XeFG context.
    const size_t xefgGenerations = reader.generations.size();
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kNative)) << game.Error();
    ASSERT_TRUE(record(40)) << game.Error();
    EXPECT_GE(reader.frames - before, AllOutputs(40, 1) - 1) << "the native swapchain must keep recording";
    EXPECT_GT(reader.generations.size(), xefgGenerations) << "the native swapchain must publish a new generation";
    before = reader.frames;
    const size_t nativeGenerations = reader.generations.size();
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kXeFG)) << game.Error();
    ASSERT_TRUE(game.SetXeFGFrameGeneration(true, 1)) << game.Error();
    ASSERT_TRUE(record(60)) << game.Error();
    EXPECT_GE(reader.frames - before, AllOutputs(60, 2) - 2) << "the returning XeFG swapchain must record every output";
    EXPECT_GT(reader.generations.size(), nativeGenerations)
        << "the returning XeFG swapchain must publish a new transport generation";
    ExpectPublished(game, kXeFGStatusType, 2, "XeFG on after returning");

    EXPECT_GT(reader.shades.size(), 12u) << "published textures must contain advancing pixels";
    EXPECT_GT(reader.timestamps.size(), reader.frames / 2);
    if (includeOverlay)
        EXPECT_GE(reader.framesWithOverlay, reader.frames - 4) << "the overlay must be on the recorded outputs";
    else
        EXPECT_EQ(reader.framesWithOverlay, 0u);
    capture(false);
    ASSERT_TRUE(reader.Consume());
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowXeFGCapture, RecordsEveryOutputWithTheOverlay) { RecordXeFG(true); }
TEST(FlowXeFGCapture, RecordsEveryOutputWithoutTheOverlay) { RecordXeFG(false); }

}  // namespace
