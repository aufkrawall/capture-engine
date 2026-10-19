#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "hook/fg/output_ratio_window.h"
#include "source_fragment_reader.h"

// The output half of the DLSS-G idle proof. Talos 20261019_182822: after FSR FG, the fresh DLSS-G
// swapchain presented every output from Streamline's thread, so only the title's PresentStart markers
// bound application frames, and those run asynchronously to the outputs.
namespace {

using ce::fg_runtime::ApplicationFrameSource;
using ce::fg_runtime::OutputRatioWindow;

constexpr uint32_t kWindow = OutputRatioWindow::kWindowFrames;

// Feeds one frame per entry with that many outputs before its boundary; returns the last verdict.
bool Feed(OutputRatioWindow& window, const std::vector<uint32_t>& outputsPerFrame,
          ApplicationFrameSource source = ApplicationFrameSource::kPresentHook) {
    for (const uint32_t outputs : outputsPerFrame) {
        for (uint32_t i = 0; i < outputs; ++i)
            window.NoteOutput();
        window.NoteFrame(source);
    }
    return window.Single();
}

std::vector<uint32_t> Repeat(std::vector<uint32_t> pattern, uint32_t frames) {
    std::vector<uint32_t> out;
    while (out.size() < frames)
        out.push_back(pattern[out.size() % pattern.size()]);
    return out;
}

TEST(OutputRatioWindowTest, OneOutputPerFrameIsSingleAfterAWholeWindow) {
    OutputRatioWindow window;
    EXPECT_FALSE(Feed(window, Repeat({1}, kWindow - 1)));
    EXPECT_TRUE(Feed(window, {1}));
}

TEST(OutputRatioWindowTest, BoundaryJitterDoesNotBreakAnIdleRatio) {
    // An output landing across the asynchronous frame boundary reads 2,0 instead of 1,1.
    OutputRatioWindow window;
    EXPECT_TRUE(Feed(window, Repeat({2, 0, 1, 1, 1, 2, 0, 1}, kWindow)));
    EXPECT_TRUE(Feed(window, Repeat({1, 2, 0, 1}, kWindow * 4))) << "never flaps while idle";
}

TEST(OutputRatioWindowTest, GenerationIsNeverSingleWhateverItsJitter) {
    OutputRatioWindow twoX;
    EXPECT_FALSE(Feed(twoX, Repeat({1, 3, 2, 2}, kWindow * 3)));
    OutputRatioWindow threeX;
    EXPECT_FALSE(Feed(threeX, Repeat({3}, kWindow * 3)));
}

TEST(OutputRatioWindowTest, ResumedGenerationEndsSingleWithinAFewFrames) {
    OutputRatioWindow window;
    ASSERT_TRUE(Feed(window, Repeat({1}, kWindow)));
    uint32_t frames = 0;
    while (window.Single() && frames < kWindow) {
        Feed(window, {2});
        ++frames;
    }
    EXPECT_FALSE(window.Single());
    EXPECT_LE(frames, 4u);
}

TEST(OutputRatioWindowTest, UncountedOutputsAreNoProof) {
    OutputRatioWindow window;
    EXPECT_FALSE(Feed(window, Repeat({0}, kWindow * 2)));
    EXPECT_FALSE(Feed(window, Repeat({1, 0}, kWindow * 2))) << "half the outputs missing";
}

TEST(OutputRatioWindowTest, TitleMarkersOwnTheFrameCountOnceSeen) {
    OutputRatioWindow window;
    ASSERT_TRUE(Feed(window, Repeat({1}, kWindow)));
    const auto first = window.NoteFrame(ApplicationFrameSource::kTitleMarker);
    EXPECT_TRUE(first.changed) << "a new frame source restarts the proof";
    EXPECT_FALSE(window.Single());
    EXPECT_TRUE(window.TitleMarkers());

    // 2x with a present-hook AND a marker boundary per frame: only the markers count.
    for (uint32_t frame = 0; frame < kWindow * 2; ++frame) {
        window.NoteOutput();
        window.NoteOutput();
        EXPECT_FALSE(window.NoteFrame(ApplicationFrameSource::kPresentHook).changed);
        window.NoteFrame(ApplicationFrameSource::kTitleMarker);
    }
    EXPECT_FALSE(window.Single());

    EXPECT_TRUE(Feed(window, Repeat({1}, kWindow), ApplicationFrameSource::kTitleMarker));
    window.Reset();
    EXPECT_FALSE(window.Single());
    EXPECT_FALSE(window.TitleMarkers());
}

TEST(OutputRatioWindowTest, VerdictsReportTheDecidingWindow) {
    OutputRatioWindow window;
    OutputRatioWindow::Verdict verdict;
    for (uint32_t frame = 0; frame < kWindow; ++frame) {
        window.NoteOutput();
        verdict = window.NoteFrame(ApplicationFrameSource::kPresentHook);
    }
    EXPECT_TRUE(verdict.changed);
    EXPECT_TRUE(verdict.single);
    EXPECT_EQ(verdict.outputs, kWindow);
    EXPECT_EQ(verdict.frames, kWindow);
    for (uint32_t frame = 0; frame < kWindow; ++frame) {
        window.NoteOutput();
        EXPECT_FALSE(window.NoteFrame(ApplicationFrameSource::kPresentHook).changed) << "unchanged verdicts are silent";
    }
}

std::string ReadSource(const char* relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

// Frames come from the title's own PresentStart (not CE-issued markers) and the DXGI present hook;
// outputs are counted exactly where the final-output timeline counts them into its source group.
TEST(OutputRatioWindowWiringTest, FramesAndOutputsAreFedWhereTheyHappen) {
    const std::string pcl = ReadSource("hook/streamline/streamline_hook_pcl.cpp");
    const size_t titleBranch = pcl.find("!StreamlineHook::CeIssuedFrameMarkerScope::Active()");
    const size_t marker =
        pcl.find("g_FGCompat.NoteApplicationFrame(ce::fg_runtime::ApplicationFrameSource::kTitleMarker)", titleBranch);
    ASSERT_NE(titleBranch, std::string::npos);
    EXPECT_NE(marker, std::string::npos);

    const std::string output = ReadSource("hook/d3d12/dx12_hook_final_output_capture.cpp");
    EXPECT_NE(output.find("g_FGCompat.NoteApplicationFrame(ce::fg_runtime::ApplicationFrameSource::kPresentHook)"),
              std::string::npos);
    const size_t next = output.find("ce::capture_policy::NextFinalOutputTimestampQpc(");
    ASSERT_NE(next, std::string::npos);
    const size_t counted = output.find("g_FGCompat.NoteRuntimeOutput();", next);
    ASSERT_NE(counted, std::string::npos);
    EXPECT_LT(counted - next, 200u) << "count beside the timeline's own count";
    const size_t skipped = output.find("void DX12_NoteSkippedStreamlineFinalOutput()");
    ASSERT_NE(skipped, std::string::npos);
    EXPECT_LT(output.find("g_FGCompat.NoteRuntimeOutput();", skipped),
              output.find("DX12FinalOutputCapturePlan", skipped));
}

}  // namespace
