#include "test_fps_limiter_shared.h"

#include <filesystem>
#include <initializer_list>
#include <string>

#include "hook/pacing/streamline_output_site.h"
#include "source_fragment_reader.h"

// Streamline's own Presents on the pre-FSR normal DXGI route (Talos 20261019_204226, GTA V Enhanced): the game
// Presents into Streamline's proxy, so every Present at CE's hook is one of Streamline's outputs. Paced as
// application Presents they had no output-group admission; they are the final-output boundary now, except on a
// swapchain where the application's own Present is paced too.
namespace {

using ce::fps_limiter_policy::PresentSite;
using ce::streamline_output_site::ApplicationPacedSwapchains;
using ce::streamline_output_site::IsApplicationPresent;
using ce::streamline_output_site::IsFinalOutputBoundary;

TEST(StreamlineOutputSiteTest, OnlyAStandaloneStreamlinePresentUnderDlssgIsTheBoundary) {
    EXPECT_TRUE(IsFinalOutputBoundary(true, true, true, false));
    EXPECT_FALSE(IsFinalOutputBoundary(false, true, true, false)) << "DLSS-G presents D3D12 swapchains";
    EXPECT_FALSE(IsFinalOutputBoundary(true, false, true, false))
        << "DLSS-G off: Streamline forwards the game's frame 1:1, the application-present site fits";
    EXPECT_FALSE(IsFinalOutputBoundary(true, true, false, false)) << "not issued by Streamline";
    EXPECT_FALSE(IsFinalOutputBoundary(true, true, true, true))
        << "the application's own Present is paced on this swapchain: a second pacer would admit partial groups";
}

TEST(StreamlineOutputSiteTest, ApplicationPresentExcludesEveryRuntimeOutput) {
    EXPECT_TRUE(IsApplicationPresent(false, false, false));
    EXPECT_FALSE(IsApplicationPresent(true, false, false)) << "Streamline's own Present";
    EXPECT_FALSE(IsApplicationPresent(false, true, false)) << "AMD's FSR FG presenter";
    EXPECT_FALSE(IsApplicationPresent(false, false, true)) << "a callback-proven runtime output";
}

TEST(StreamlineOutputSiteTest, ApplicationPacedRecordIsPerSwapchainAndFailsTowardTheApplicationSite) {
    ApplicationPacedSwapchains record;
    int swapchains[ApplicationPacedSwapchains::kCapacity + 1] = {};
    EXPECT_FALSE(record.Contains(&swapchains[0]));
    EXPECT_FALSE(record.Contains(nullptr));
    EXPECT_TRUE(record.Note(&swapchains[0]));
    EXPECT_FALSE(record.Note(&swapchains[0])) << "one line per swapchain";
    EXPECT_TRUE(record.Contains(&swapchains[0]));
    EXPECT_FALSE(record.Contains(&swapchains[1])) << "another swapchain keeps its own verdict";
    EXPECT_FALSE(record.Note(nullptr));
    for (size_t i = 1; i < ApplicationPacedSwapchains::kCapacity; ++i) {
        EXPECT_TRUE(record.Note(&swapchains[i]));
    }
    EXPECT_FALSE(record.Contains(&swapchains[ApplicationPacedSwapchains::kCapacity]));
    EXPECT_TRUE(record.Note(&swapchains[ApplicationPacedSwapchains::kCapacity]));
    int unrelated = 0;
    EXPECT_TRUE(record.Contains(&unrelated)) << "a full record answers paced: never a second pacer";
}

std::string ReadHook(const char* fileName) {
    return ce::test_source::ReadFile(ce::test_source::FindSource("hook", fileName));
}

// Both DXGI normal routes feed the Streamline verdict into the site, after the routing that bypasses them.
TEST(StreamlineOutputSiteTest, BothNormalRoutesPaceStreamlineOutputsThroughTheVerdict) {
    const std::string core = ReadHook("dxgi_shared_present_core.cpp");
    const size_t verdict = core.find("IsStreamlineFinalOutputForLimiter(");
    const size_t site = core.find("ResolveDxgiPresentSite(\n                                 runtimeOutput, "
                                  "IsStreamlineFinalOutputForLimiter(");
    ASSERT_NE(verdict, std::string::npos);
    EXPECT_NE(site, std::string::npos);
    EXPECT_NE(core.find("ctx.callerFromStreamlineModule,", verdict), std::string::npos);
    EXPECT_NE(core.find("ctx.callerFromFFXFrameGenerationModule, runtimeOutput)));", verdict), std::string::npos);

    const std::string present1 = ReadHook("dxgi_shared_present1.cpp");
    const size_t present1Verdict =
        present1.find("const bool slOutputBoundary = IsStreamlineFinalOutputForLimiter(");
    ASSERT_NE(present1Verdict, std::string::npos);
    EXPECT_LT(present1Verdict,
              present1.find("ResolveDxgiPresentSite(callbackProvenRuntimeOutput, slOutputBoundary)"));
}

TEST(StreamlineOutputSiteTest, TheWrapperRecordsItsPacedSwapchainBeforeItsLimiterStage) {
    const std::string wrapper =
        ce::test_source::ReadFile(ce::test_source::FindSource("hook", "dxgi_swapchain_wrap_present.cpp"));
    for (const char* real : {"pRealCached", "pReal1Cached"}) {
        const std::string note = std::string("DXGIShared::NoteApplicationPresentPacedOnSwapchain(") + real +
                                 ", \"CE's swapchain wrapper\");";
        const size_t at = wrapper.find(note);
        ASSERT_NE(at, std::string::npos) << real;
        EXPECT_EQ(wrapper.find(std::string("DXGIShared::ApplyFpsLimiterStage(") + real + ", true,", at),
                  wrapper.find('\n', at) + 5)
            << real << ": the record precedes the limiter stage";
    }
    const std::string stage = ReadHook("dxgi_limiter_stage.cpp");
    const size_t record = stage.find("RecordApplicationPresentPaced(pSwapChain, \"the application's own Present\");");
    ASSERT_NE(record, std::string::npos);
    EXPECT_LT(record, stage.find("s_applicationPacedSwapchains.Contains(pSwapChain)"))
        << "an application Present is recorded before any verdict on that swapchain";
}

// What the boundary buys when CE paces the outputs itself (basic limiter, or the Reflex timer fallback): Talos's
// flip-metered 4x burst (+0, +1.0, +1.4, +2.5 ms) under a 140 cap is one admitted output per frame on the
// 35 fps group grid; the generated outputs pass without a wait.
class StreamlineOutputPacingTest : public FpsLimiterTest {
protected:
    void SetUp() override {
        FpsLimiterTest::SetUp();
        UseVirtualClock();
        mockShm->fpsLimiter.SetGeneralEnabled(true);
        mockShm->fpsLimiter.SetGeneralFps(140);
        mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));
        g_FGCompat.SetDLSSFGMultiplier(4);
        g_FGCompat.SetDLSSFGActive(true);
        ConfirmDLSSFGPacing();
    }
    void TearDown() override {
        g_FGCompat.SetDLSSFGActive(false);
        FpsLimiterTest::TearDown();
    }

    // One game frame: the burst, then 3 ms of game work until the next frame's first output.
    int64_t BurstFrame(PresentSite site) {
        return VirtualTicksOf([&] {
            for (const int64_t untilNextUs : {1000, 400, 1100, 3000}) {
                limiter.Apply(true, site);
                clock.Advance(TicksFromUs(untilNextUs));
            }
        });
    }
};

TEST_F(StreamlineOutputPacingTest, BoundaryPacesOneGroupPerBaseIntervalOnMfgBursts) {
    for (int frame = 0; frame < 4; ++frame) {
        BurstFrame(PresentSite::kFinalOutputBoundary);
    }
    const uint32_t pacedBefore = limiter.GetPacedGroupCount();
    const uint32_t passedBefore = limiter.GetGeneratedSlotPassCount();
    int64_t ticks = 0;
    for (int frame = 0; frame < 8; ++frame) {
        ticks += BurstFrame(PresentSite::kFinalOutputBoundary);
    }
    EXPECT_EQ(limiter.GetPacedGroupCount() - pacedBefore, 8u) << "one paced output per game frame";
    EXPECT_EQ(limiter.GetGeneratedSlotPassCount() - passedBefore, 24u) << "the generated outputs never wait";
    EXPECT_NEAR(static_cast<double>(ticks), static_cast<double>(FirstIntervalTicks(140, 4)) * 8,
                static_cast<double>(TicksFromUs(10)))
        << "the game frame takes one 4-output group interval at 140 fps: a 35 fps base";
}

}  // namespace
