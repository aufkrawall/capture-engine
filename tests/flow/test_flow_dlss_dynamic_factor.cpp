// Recording a DLSS frame generation game whose factor as CE reads it changes per game frame while the output rate
// holds, as dynamic MFG can do. W3 at 138 fps output (session 20261106_195839) read false 2x/4x flips up to 67 times a
// second, and the recording's repeated frames followed them (r=0.91 over 94 s; about 2 repeats/s at <=10 flips/s, about
// 20 at >=30). CE's final-output clock must stamp what the runtime does: the same output spacing whatever the factor.

#include "tests/flow/flow_capture_reader.h"
#include "tests/flow/flow_test_support.h"

#include <algorithm>

namespace {

using namespace ce::flow;

GameOptions StreamlineGameOptions() {
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kStreamline;
    return options;
}

// The recorded outputs from `first` on are spaced `expectedUs` apart, with half an interval of slack.
void ExpectEvenlySpaced(const CaptureReader& reader, size_t first, double expectedUs, FlowGame& game) {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    double minimumUs = 1e12;
    double maximumUs = 0.0;
    for (size_t i = first + 1; i < reader.timeline.size(); ++i) {
        const double us = static_cast<double>(reader.timeline[i] - reader.timeline[i - 1]) * 1e6 /
                          static_cast<double>(frequency.QuadPart);
        minimumUs = (std::min)(minimumUs, us);
        maximumUs = (std::max)(maximumUs, us);
    }
    ASSERT_GT(reader.timeline.size(), first + 8);
    EXPECT_GE(minimumUs, expectedUs * 0.5) << "outputs stamped together; logs: " << game.LogDirectory();
    EXPECT_LE(maximumUs, expectedUs * 1.5) << "a stamped gap the runtime did not present; logs: " << game.LogDirectory();
}

struct Recording {
    explicit Recording(FlowGame& game) : game_(game), memory_(game.CaptureMemory()) {
        capture_ = reinterpret_cast<void (*)(bool)>(
            GetProcAddress(GetModuleHandleW(L"capture_hook_x64.dll"), "CEFlow_SetInjectCapture"));
        capture_(true);
        memory_->fpsLimiter.SetCaptureFps(0);  // observe every output, independently of the CFR cadence gate
        reader = std::make_unique<CaptureReader>(game);
    }
    ~Recording() { capture_(false); }
    bool Record(int frames) {
        for (int frame = 0; frame < frames; ++frame) {
            if (!game_.RenderFrame() || !reader->Consume())
                return false;
        }
        return true;
    }
    std::unique_ptr<CaptureReader> reader;

private:
    FlowGame& game_;
    SharedMemoryLayout* memory_;
    void (*capture_)(bool) = nullptr;
};

TEST(FlowDLSSDynamicFactor, FactorFlipsKeepTheRecordedOutputsEvenlySpaced) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(StreamlineGameOptions())) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true, 3)) << game.Error();
    constexpr int64_t kOutputIntervalUs = FlowGame::kFrameIntervalMicroseconds / 2;
    ASSERT_TRUE(game.SetDLSSConstantOutputInterval(kOutputIntervalUs)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();

    Recording recording(game);
    ASSERT_TRUE(recording.Record(30)) << game.Error();
    const size_t settled = recording.reader->timeline.size();
    for (int frame = 0; frame < 120; ++frame) {
        ASSERT_TRUE(game.SetDLSSFrameGeneration(true, frame % 2 == 0 ? 1u : 3u)) << game.Error();
        ASSERT_TRUE(recording.Record(1)) << game.Error();
    }
    EXPECT_GE(recording.reader->frames, settled + 300u) << "every output of both factors; logs: " << game.LogDirectory();
    ExpectEvenlySpaced(*recording.reader, settled + 8, static_cast<double>(kOutputIntervalUs), game);

    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

// A fixed factor changed in a menu moves the output rate instead: the clock must follow it once the rate is measured.
TEST(FlowDLSSDynamicFactor, AFixedFactorChangeReachesTheNewOutputSpacing) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(StreamlineGameOptions())) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true, 1)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();

    Recording recording(game);
    ASSERT_TRUE(recording.Record(60)) << game.Error();
    ExpectEvenlySpaced(*recording.reader, 8, FlowGame::kFrameIntervalMicroseconds / 2.0, game);

    ASSERT_TRUE(game.SetDLSSFrameGeneration(true, 3)) << game.Error();
    ASSERT_TRUE(recording.Record(288)) << game.Error();  // two seconds of game frames to measure the new rate
    const size_t settled = recording.reader->timeline.size();
    ASSERT_TRUE(recording.Record(60)) << game.Error();
    ExpectEvenlySpaced(*recording.reader, settled, FlowGame::kFrameIntervalMicroseconds / 4.0, game);

    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

}  // namespace
