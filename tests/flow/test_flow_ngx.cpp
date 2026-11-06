#include "tests/flow/flow_host_ngx.h"
#include "tests/flow/flow_test_support.h"

namespace {
using namespace ce::flow;

TEST(FlowNGX, NeuralRenderingDoesNotActivateFrameGeneration) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    NGXRuntime runtime(game);
    ASSERT_TRUE(runtime.Ready());
    void* sr = runtime.Create(1);
    ASSERT_NE(sr, nullptr);
    ASSERT_TRUE(runtime.Evaluate(sr));
    ASSERT_TRUE(game.PublishedNGX().srActive);

    // Metro 2033 Redux uses SR and feature 18 (DLSS NR), without a frame generator.
    // Even a reused parameter block carrying an FG factor cannot make NR generate frames.
    for (const int generatedFrames : {0, 3}) {
        SCOPED_TRACE(generatedFrames);
        runtime.SetGeneratedFrames(generatedFrames);
        void* nr = runtime.Create(18);
        ASSERT_NE(nr, nullptr);
        EXPECT_FALSE(game.PublishedNGX().fgActive) << "NR creation is not FG evidence";
        EXPECT_LT(game.PublishedNGX().fgMultiplier, 2);
        ASSERT_TRUE(runtime.Evaluate(nr));
        EXPECT_FALSE(game.PublishedNGX().fgActive) << "NR evaluation is not FG evidence";
        EXPECT_LT(game.PublishedNGX().fgMultiplier, 2);
        EXPECT_TRUE(game.PublishedNGX().srActive);
        EXPECT_FALSE(game.PublishedNGX().rrActive);
        ASSERT_TRUE(runtime.Release(nr));
    }
    ASSERT_TRUE(runtime.Release(sr));
    ASSERT_TRUE(runtime.Close());
    EXPECT_EQ(runtime.Counters().liveFeatures, 0u);
    EXPECT_EQ(runtime.Counters().liveParameters, 0u);
    ASSERT_TRUE(game.RenderFrames(16)) << game.Error();
    ExpectPublished(game, 0, 0, "SR and NR without FG");
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowNGX, FeaturePublicationFollowsSuccessfulEvaluationAndRelease) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    NGXRuntime runtime(game);
    ASSERT_TRUE(runtime.Ready());
    runtime.Fail(ngx::Failure::kCreate);
    EXPECT_EQ(runtime.Create(13), nullptr);
    EXPECT_FALSE(game.PublishedNGX().rrActive);
    runtime.Fail(ngx::Failure::kNone);

    void* sr = runtime.Create(1);
    void* rr = runtime.Create(13);
    ASSERT_NE(sr, nullptr);
    ASSERT_NE(rr, nullptr);
    EXPECT_FALSE(game.PublishedNGX().srActive);
    EXPECT_FALSE(game.PublishedNGX().rrActive);
    runtime.Fail(ngx::Failure::kEvaluate);
    EXPECT_FALSE(runtime.Evaluate(sr));
    EXPECT_FALSE(game.PublishedNGX().srActive);
    runtime.Fail(ngx::Failure::kNone);
    ASSERT_TRUE(runtime.Evaluate(sr));
    EXPECT_TRUE(game.PublishedNGX().srActive);
    EXPECT_FALSE(game.PublishedNGX().rrActive);

    // Releasing an unevaluated handle cannot displace the currently published feature.
    ASSERT_TRUE(runtime.Release(rr));
    EXPECT_TRUE(game.PublishedNGX().srActive);
    rr = runtime.Create(13);
    ASSERT_NE(rr, nullptr);
    ASSERT_TRUE(runtime.Evaluate(rr));
    EXPECT_FALSE(game.PublishedNGX().srActive);
    EXPECT_TRUE(game.PublishedNGX().rrActive);
    runtime.Fail(ngx::Failure::kRelease);
    EXPECT_FALSE(runtime.Release(rr));
    EXPECT_TRUE(game.PublishedNGX().rrActive);
    runtime.Fail(ngx::Failure::kNone);
    ASSERT_TRUE(runtime.Release(rr));
    EXPECT_TRUE(game.PublishedNGX().srActive);
    EXPECT_FALSE(game.PublishedNGX().rrActive);
    ASSERT_TRUE(runtime.Release(sr));
    EXPECT_FALSE(game.PublishedNGX().srActive);

    // The fake deterministically reuses the released address for a different feature.
    rr = runtime.Create(13);
    ASSERT_EQ(rr, sr);
    EXPECT_FALSE(game.PublishedNGX().rrActive);
    ASSERT_TRUE(runtime.Evaluate(rr));
    EXPECT_FALSE(game.PublishedNGX().srActive);
    EXPECT_TRUE(game.PublishedNGX().rrActive);
    ASSERT_TRUE(runtime.Release(rr));
    EXPECT_FALSE(game.PublishedNGX().rrActive);
    ASSERT_TRUE(runtime.Close());
    const auto counts = runtime.Counters();
    EXPECT_EQ(counts.creates, 5u);
    EXPECT_EQ(counts.evaluations, 4u);
    EXPECT_EQ(counts.releases, 5u);
    EXPECT_EQ(counts.liveFeatures, 0u);
    EXPECT_EQ(counts.liveParameters, 0u);
    ASSERT_TRUE(game.RenderFrames(16)) << game.Error();
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowNGX, InFlightEvaluationCannotOverrideExplicitStreamlineOffAndCanReactivate) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kStreamline;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    // Establish the native route with one completed output before testing the OFF/ON handover.
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    ExpectEveryPresentCoveredOnce(game);
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    NGXRuntime runtime(game);
    ASSERT_TRUE(runtime.Ready());
    runtime.SetGeneratedFrames(1);
    void* fg = runtime.Create(9);
    ASSERT_NE(fg, nullptr);
    ASSERT_TRUE(runtime.Evaluate(fg));
    ExpectPublished(game, 1, 2, "NGX evaluation while enabled");

    ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
    ASSERT_FALSE(game.DLSSFrameGenerationRunning());
    EXPECT_FALSE(game.PublishedNGX().fgActive);
    ASSERT_TRUE(runtime.Evaluate(fg));
    // Inspect immediately: a subsequent state poll or Present must not mask erroneous reactivation.
    EXPECT_FALSE(game.PublishedNGX().fgActive);
    EXPECT_LT(game.PublishedNGX().fgMultiplier, 2);
    ASSERT_TRUE(game.RenderFrames(32)) << game.Error();
    ExpectPublished(game, 0, 0, "OFF remains visible");

    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(runtime.Evaluate(fg));
    EXPECT_TRUE(game.PublishedNGX().fgActive);
    EXPECT_EQ(game.PublishedNGX().fgMultiplier, 2);
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    ExpectPublished(game, 1, 2, "NGX reactivation visible after outputs");
    ASSERT_TRUE(runtime.Release(fg));
    ASSERT_TRUE(runtime.Close());
    EXPECT_EQ(runtime.Counters().evaluations, 3u);
    EXPECT_EQ(runtime.Counters().liveFeatures, 0u);
    EXPECT_EQ(runtime.Counters().liveParameters, 0u);
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowNGX, FeatureCreationCannotOverrideAcceptedOffForAnyFGFeature) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kStreamline;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    NGXRuntime runtime(game);
    ASSERT_TRUE(runtime.Ready());
    runtime.SetGeneratedFrames(2);
    ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
    ASSERT_FALSE(game.DLSSFrameGenerationRunning());
    ASSERT_FALSE(game.PublishedNGX().fgActive);
    for (const int feature : {9, 11, 18}) {
        void* handle = runtime.Create(feature);
        ASSERT_NE(handle, nullptr) << "feature " << feature;
        EXPECT_FALSE(game.PublishedNGX().fgActive) << "creation after accepted OFF, feature " << feature;
        EXPECT_LT(game.PublishedNGX().fgMultiplier, 2) << "feature " << feature;
        ASSERT_TRUE(runtime.Release(handle));
    }
    ASSERT_TRUE(runtime.Close());
    EXPECT_EQ(runtime.Counters().creates, 3u);
    EXPECT_EQ(runtime.Counters().evaluations, 0u);
    EXPECT_EQ(runtime.Counters().liveFeatures, 0u);
    EXPECT_EQ(runtime.Counters().liveParameters, 0u);
    ASSERT_TRUE(game.RenderFrames(32)) << game.Error();
    ExpectPublished(game, 0, 0, "OFF after feature creation");
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowNGX, CreationAndEvaluationPreserveObservedFactorsAndLegacyLatch) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    NGXRuntime runtime(game);
    ASSERT_TRUE(runtime.Ready());
    for (const int feature : {9, 11}) {
        runtime.SetGeneratedFrames(2);
        void* handle = runtime.Create(feature);
        ASSERT_NE(handle, nullptr);
        EXPECT_TRUE(game.PublishedNGX().fgActive);
        EXPECT_EQ(game.PublishedNGX().fgMultiplier, 3) << "creation, feature " << feature;
        runtime.SetGeneratedFrames(3);
        ASSERT_TRUE(runtime.Evaluate(handle));
        EXPECT_EQ(game.PublishedNGX().fgMultiplier, 4) << "evaluation, feature " << feature;
        runtime.SetGeneratedFrames(0);
        ASSERT_TRUE(runtime.Evaluate(handle));
        EXPECT_EQ(game.PublishedNGX().fgMultiplier, 4) << "missing factor is not new activity evidence";
        ASSERT_TRUE(runtime.Release(handle));
    }
    runtime.SetGeneratedFrames(0);
    void* legacy = runtime.Create(9);
    ASSERT_NE(legacy, nullptr);
    EXPECT_EQ(game.PublishedNGX().fgMultiplier, 4) << "legacy default must preserve a latched factor";
    ASSERT_TRUE(runtime.Release(legacy));
    ASSERT_TRUE(runtime.Close());
    EXPECT_EQ(runtime.Counters().creates, 3u);
    EXPECT_EQ(runtime.Counters().evaluations, 4u);
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

// W3 with dlss_fg_mode=dynamic (session 20261106_195839): the game asked for one generated frame at every poll on its
// own thread while the runtime evaluated three on the presenter thread, and the status flipped between 2x and 4x 3680
// times in three minutes with every game frame still 4 outputs.
TEST(FlowNGX, TheRuntimesEvaluationOutranksTheGamesPolledRequest) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kStreamline;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true, 1)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    EXPECT_EQ(game.PublishedFG().multiplier, 2) << "the request stands before any evaluation";

    NGXRuntime runtime(game);
    ASSERT_TRUE(runtime.Ready());
    void* fg = runtime.Create(11);
    ASSERT_NE(fg, nullptr);
    runtime.SetGeneratedFrames(3);
    for (int frame = 0; frame < 60; ++frame) {
        ASSERT_TRUE(runtime.Evaluate(fg));
        ASSERT_TRUE(game.RenderFrame()) << game.Error();  // ends with the game's slDLSSGGetState poll
        ASSERT_EQ(game.PublishedFG().multiplier, 4) << "frame " << frame << "; logs: " << game.LogDirectory();
        ASSERT_EQ(game.PublishedNGX().fgMultiplier, 4) << "frame " << frame;
    }

    // A request the game changes may be what the runtime does next, until the runtime says otherwise.
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true, 2)) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    EXPECT_EQ(game.PublishedFG().multiplier, 3) << "logs: " << game.LogDirectory();
    ASSERT_TRUE(runtime.Evaluate(fg));
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    EXPECT_EQ(game.PublishedFG().multiplier, 4) << "logs: " << game.LogDirectory();

    // Off forgets the runtime's factor: the next activation starts from what the game asks for.
    ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(120)) << game.Error();
    EXPECT_LT(game.PublishedFG().multiplier, 2) << "logs: " << game.LogDirectory();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true, 1)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    EXPECT_EQ(game.PublishedFG().multiplier, 2) << "logs: " << game.LogDirectory();

    ASSERT_TRUE(runtime.Release(fg));
    ASSERT_TRUE(runtime.Close());
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

}  // namespace
