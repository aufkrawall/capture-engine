#include <gtest/gtest.h>

#include <filesystem>
#include <limits>
#include <string>

#include "../hook/common/dx12_overlay_policy/ffx_topmost_batch.h"

#include "source_fragment_reader.h"

namespace {

using ce::dx12_overlay_policy::AdvanceFinalECLBatchSignatureStability;
using ce::dx12_overlay_policy::FinalECLBatchSignature;
using ce::dx12_overlay_policy::HasCompletedNoCallbackTopmostActivation;
using ce::dx12_overlay_policy::ShouldAppendTopmostOverlayToFinalECLBatch;
using ce::dx12_overlay_policy::ShouldGrantNoCallbackTopmostOwnership;
using ce::dx12_overlay_policy::ShouldRenderAppCallbackTopmostOverlay;
using ce::dx12_overlay_policy::ShouldSampleFrameTimingFromFFXPresentCallback;
using ce::dx12_overlay_policy::ShouldYieldFFXPresentCallbackToTopmostRoute;

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

TEST(FFXTopmostBatchPolicyTest, RequiresTwoConsecutiveIdenticalFinalBatches) {
    const FinalECLBatchSignature target{0x1234, 0x9876, 2};

    EXPECT_EQ(AdvanceFinalECLBatchSignatureStability({}, 0, target), 1u);
    EXPECT_FALSE(ShouldAppendTopmostOverlayToFinalECLBatch(
        true, 1, target, target.callSite, target.queueIdentity, target.ordinal, 2, 129));
    EXPECT_EQ(AdvanceFinalECLBatchSignatureStability(target, 1, target), 2u);
    EXPECT_TRUE(ShouldAppendTopmostOverlayToFinalECLBatch(
        true, 2, target, target.callSite, target.queueIdentity, target.ordinal, 2, 129));
}

TEST(FFXTopmostBatchPolicyTest, SignatureChangeRestartsLearning) {
    const FinalECLBatchSignature previous{0x1234, 0x9876, 2};

    EXPECT_EQ(AdvanceFinalECLBatchSignatureStability(previous, 37, {0x5678, 0x9876, 2}), 1u);
    EXPECT_EQ(AdvanceFinalECLBatchSignatureStability(previous, 37, {0x1234, 0x6789, 2}), 1u);
    EXPECT_EQ(AdvanceFinalECLBatchSignatureStability(previous, 37, {0x1234, 0x9876, 3}), 1u);
    EXPECT_EQ(AdvanceFinalECLBatchSignatureStability(previous, 37, {}), 0u);
}

TEST(FFXTopmostBatchPolicyTest, StabilityCounterSaturates) {
    const FinalECLBatchSignature target{0x1234, 0x9876, 1};
    EXPECT_EQ(AdvanceFinalECLBatchSignatureStability(
                  target, std::numeric_limits<uint32_t>::max(), target),
              std::numeric_limits<uint32_t>::max());
}

TEST(FFXTopmostBatchPolicyTest, RequiresExactCallSiteAndOrdinal) {
    const FinalECLBatchSignature target{0x1234, 0x9876, 3};

    EXPECT_FALSE(ShouldAppendTopmostOverlayToFinalECLBatch(true, 2, target, 0x5678, 0x9876, 3, 1, 129));
    EXPECT_FALSE(ShouldAppendTopmostOverlayToFinalECLBatch(true, 2, target, 0x1234, 0x6789, 3, 1, 129));
    EXPECT_FALSE(ShouldAppendTopmostOverlayToFinalECLBatch(true, 2, target, 0x1234, 0x9876, 2, 1, 129));
    EXPECT_TRUE(ShouldAppendTopmostOverlayToFinalECLBatch(true, 2, target, 0x1234, 0x9876, 3, 1, 129));
}

TEST(FFXTopmostBatchPolicyTest, RefusesInvalidOrUnrepresentableBatch) {
    const FinalECLBatchSignature target{0x1234, 0x9876, 1};

    EXPECT_FALSE(ShouldAppendTopmostOverlayToFinalECLBatch(false, 2, target, 0x1234, 0x9876, 1, 1, 129));
    EXPECT_FALSE(ShouldAppendTopmostOverlayToFinalECLBatch(true, 2, {}, 0x1234, 0x9876, 1, 1, 129));
    EXPECT_FALSE(ShouldAppendTopmostOverlayToFinalECLBatch(true, 2, target, 0x1234, 0x9876, 1, 0, 129));
    EXPECT_FALSE(ShouldAppendTopmostOverlayToFinalECLBatch(true, 2, target, 0x1234, 0x9876, 1, 129, 129));
}

TEST(FFXTopmostBatchPolicyTest, CallbackYieldsOnlyToAProvenLaterTopmostRoute) {
    EXPECT_FALSE(ShouldYieldFFXPresentCallbackToTopmostRoute(
        /*nativeNoCallbackComposition=*/false, /*belowForeignTopmostSubmitProven=*/false,
        /*completedNoCallbackTopmostBatch=*/false));
    EXPECT_TRUE(ShouldYieldFFXPresentCallbackToTopmostRoute(
        /*nativeNoCallbackComposition=*/false, /*belowForeignTopmostSubmitProven=*/true,
        /*completedNoCallbackTopmostBatch=*/false));
    EXPECT_FALSE(ShouldYieldFFXPresentCallbackToTopmostRoute(
        /*nativeNoCallbackComposition=*/true, /*belowForeignTopmostSubmitProven=*/true,
        /*completedNoCallbackTopmostBatch=*/false));
    EXPECT_TRUE(ShouldYieldFFXPresentCallbackToTopmostRoute(
        /*nativeNoCallbackComposition=*/true, /*belowForeignTopmostSubmitProven=*/false,
        /*completedNoCallbackTopmostBatch=*/true));
}

TEST(FFXTopmostBatchPolicyTest, NoCallbackOwnershipRequiresCompletedProbeAndRetiredBaseline) {
    EXPECT_FALSE(HasCompletedNoCallbackTopmostActivation(false, true, true));
    EXPECT_FALSE(HasCompletedNoCallbackTopmostActivation(true, false, true));
    EXPECT_FALSE(HasCompletedNoCallbackTopmostActivation(true, true, false));
    EXPECT_TRUE(HasCompletedNoCallbackTopmostActivation(true, true, true));

    EXPECT_FALSE(ShouldGrantNoCallbackTopmostOwnership(false, true));
    EXPECT_FALSE(ShouldGrantNoCallbackTopmostOwnership(true, false));
    EXPECT_TRUE(ShouldGrantNoCallbackTopmostOwnership(true, true));
}

TEST(FFXTopmostBatchPolicyTest, AppCallbackTopmostRouteStartsWithNonVisibleProbe) {
    EXPECT_FALSE(ShouldRenderAppCallbackTopmostOverlay(/*routeArmed=*/false));
    EXPECT_TRUE(ShouldRenderAppCallbackTopmostOverlay(/*routeArmed=*/true));
}

TEST(FFXTopmostBatchPolicyTest, DeepPresentIsTheOnlyFrameTimingObserverWhenAvailable) {
    EXPECT_FALSE(ShouldSampleFrameTimingFromFFXPresentCallback(
        /*runtimeOwnsNativeFSRPresentation=*/false, /*callbackYieldsToTopmostRoute=*/false,
        /*presentInterceptedBelowForeignChain=*/false));
    EXPECT_TRUE(ShouldSampleFrameTimingFromFFXPresentCallback(
        /*runtimeOwnsNativeFSRPresentation=*/true, /*callbackYieldsToTopmostRoute=*/false,
        /*presentInterceptedBelowForeignChain=*/false));
    EXPECT_FALSE(ShouldSampleFrameTimingFromFFXPresentCallback(
        /*runtimeOwnsNativeFSRPresentation=*/true, /*callbackYieldsToTopmostRoute=*/false,
        /*presentInterceptedBelowForeignChain=*/true));
    EXPECT_FALSE(ShouldSampleFrameTimingFromFFXPresentCallback(
        /*runtimeOwnsNativeFSRPresentation=*/true, /*callbackYieldsToTopmostRoute=*/true,
        /*presentInterceptedBelowForeignChain=*/false));
}

TEST(FFXTopmostBatchSourceTest, OverlayIsLastInOneExistingExecuteCommandListsCall) {
    const std::string source = ReadSource("hook/apis/dx12_hook_ffx_topmost_batch.cpp");
    ASSERT_FALSE(source.empty());

    const size_t append = source.find("combined[context->commandListCount] = overlayCommandList;");
    const size_t oneSubmit =
        source.find("context->original(queue, context->commandListCount + 1, combined.data());", append);
    ASSERT_NE(append, std::string::npos);
    ASSERT_NE(oneSubmit, std::string::npos);
    EXPECT_LT(append, oneSubmit);
    EXPECT_NE(source.find("request.inlineCompletionMarker = true;"), std::string::npos);
    EXPECT_EQ(source.find("Signal("), std::string::npos);
}

TEST(FFXTopmostBatchSourceTest, HotPathIsGenericAndExcludesCEOwnedSubmissions) {
    const std::string source = ReadSource("hook/apis/dx12_hook_ecl.cpp");
    ASSERT_FALSE(source.empty());

    const size_t exclusion = source.find("dx12_hook_s_insideCEOverlayECLDepth == 0");
    const size_t append = source.find("DX12_TryAppendNoCallbackFSRTopmostOverlayToECL", exclusion);
    ASSERT_NE(exclusion, std::string::npos);
    ASSERT_NE(append, std::string::npos);
    EXPECT_LT(exclusion, append);
}

TEST(FFXTopmostBatchSourceTest, FallbackAndInlineRenderersCoexistAndHandoffWithoutDoubleBlend) {
    const std::string renderer = ReadSource("hook/apis/dx12_ffx_suspend_overlay.cpp");
    const std::string proxy = ReadSource("hook/apis/dx12_hook_ffx_proxy_present.cpp");
    const std::string topmostBatch = ReadSource("hook/apis/dx12_hook_ffx_topmost_batch.cpp");
    ASSERT_FALSE(renderer.empty());
    ASSERT_FALSE(proxy.empty());
    ASSERT_FALSE(topmostBatch.empty());

    EXPECT_NE(renderer.find("request.inlineCompletionMarker ? g_InlineProxyStates : g_ProxyStates"),
              std::string::npos);
    EXPECT_NE(renderer.find("WriteBufferImmediate"), std::string::npos);
    EXPECT_NE(renderer.find("const bool writesTarget = clearTransparent || renderOverlay;"),
              std::string::npos);
    EXPECT_NE(renderer.find("const size_t candidate = (firstCandidate + offset) % kFrameSlotCount;"),
              std::string::npos);
    EXPECT_NE(renderer.find("inlineCompletionObserved = true;"), std::string::npos);
    EXPECT_NE(renderer.find("void ResetInlineCompletionProof(void* proxySwapChain)"), std::string::npos);
    EXPECT_NE(proxy.find("active-ui-resource-retire-ce-pixels"), std::string::npos);
    EXPECT_NE(proxy.find("request.renderOverlay = !clearOnly;"), std::string::npos);
    EXPECT_NE(proxy.find("DX12_IsNoCallbackFSRTopmostBatchReadyForOwnership()"), std::string::npos);
    EXPECT_NE(proxy.find("DX12_SetNoCallbackFSRTopmostBatchOwnership("), std::string::npos);
    EXPECT_NE(topmostBatch.find("request.renderOverlay = renderOverlay;"), std::string::npos);
    EXPECT_NE(topmostBatch.find("no-callback-fsr-topmost-activation-probe"), std::string::npos);
    EXPECT_NE(topmostBatch.find("ResetInlineCompletionProof(g_TopmostBatchSwapChain)"), std::string::npos);
    EXPECT_EQ(topmostBatch.find("HasPendingInlineRender"), std::string::npos);
}

TEST(FFXTopmostBatchSourceTest, AppCallbackHandoffProvesRouteBeforeFirstVisibleTopmostDraw) {
    const std::string ownerQueue = ReadSource("hook/apis/dx12_hook_ffx_owner_queue.cpp");
    ASSERT_FALSE(ownerQueue.empty());

    const size_t probe = ownerQueue.find("const bool activationProbe =");
    const size_t drawGate = ownerQueue.find("request.renderOverlay = !activationProbe;", probe);
    const size_t arm = ownerQueue.find(
        "g_BelowForeignChainFSRTopmostRouteArmed.store(true, std::memory_order_release);", drawGate);
    const size_t visibleNote = ownerQueue.find("if (!activationProbe) {", arm);
    ASSERT_NE(probe, std::string::npos);
    ASSERT_NE(drawGate, std::string::npos);
    ASSERT_NE(arm, std::string::npos);
    ASSERT_NE(visibleNote, std::string::npos);
    EXPECT_LT(probe, drawGate);
    EXPECT_LT(drawGate, arm);
    EXPECT_LT(arm, visibleNote);
}

TEST(FFXTopmostBatchSourceTest, CallbackRoutingChangeImmediatelyRetiresBothTopmostRoutes) {
    const std::string source = ReadSource("hook/apis/dx12_hook_ffx.cpp");
    ASSERT_FALSE(source.empty());

    const size_t configured = source.find("void DX12_OnNativeFSRPresentCallbackRoutingConfigured(");
    const size_t appRouteReset = source.find("DX12_ResetBelowForeignChainFSRTopmostSubmitProof(", configured);
    const size_t noCallbackRouteReset = source.find("DX12_ClearNoCallbackFSRTopmostBatch(", configured);
    const size_t stateWrite = source.find("dx12_hook_g_FFXPresentCallbackBridgeExpected.store(", configured);
    ASSERT_NE(configured, std::string::npos);
    ASSERT_NE(appRouteReset, std::string::npos);
    ASSERT_NE(noCallbackRouteReset, std::string::npos);
    ASSERT_NE(stateWrite, std::string::npos);
    EXPECT_LT(appRouteReset, stateWrite);
    EXPECT_LT(noCallbackRouteReset, stateWrite);
}

TEST(FFXTopmostBatchSourceTest, CallbackDrawAndFrameTimingUseIndependentExactOwners) {
    const std::string source = ReadSource("hook/apis/dx12_hook_ffx.cpp");
    const std::string metrics = ReadSource("hook/apis/dx12_hook_ffx_metrics.cpp");
    const std::string ownerQueue = ReadSource("hook/apis/dx12_hook_ffx_owner_queue.cpp");
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(metrics.empty());
    ASSERT_FALSE(ownerQueue.empty());

    const size_t decision = source.find("const bool callbackYieldsToTopmostRoute =");
    const size_t draw = source.find(
        "if (!callbackYieldsToTopmostRoute && RenderOverlayViaFFXPresentCallback(desc))", decision);
    const size_t timingCall = source.find("DX12_UpdateFFXPresentCallbackFrameTiming(", draw);
    const size_t deepPresentObserver = metrics.find("DXGIShared::IsPresentInterceptedBelowForeignChain()");
    const size_t timingDecision = metrics.find("ShouldSampleFrameTimingFromFFXPresentCallback(", deepPresentObserver);
    const size_t timingGate = metrics.find("if (callbackSamplesFrameTiming) {", timingDecision);
    const size_t timingUpdate = metrics.find("metrics->Update(PerfLogger::GetQpcUs());", timingGate);
    ASSERT_NE(decision, std::string::npos);
    ASSERT_NE(draw, std::string::npos);
    ASSERT_NE(timingCall, std::string::npos);
    ASSERT_NE(deepPresentObserver, std::string::npos);
    ASSERT_NE(timingDecision, std::string::npos);
    ASSERT_NE(timingGate, std::string::npos);
    ASSERT_NE(timingUpdate, std::string::npos);
    EXPECT_LT(decision, draw);
    EXPECT_LT(draw, timingCall);
    EXPECT_LT(deepPresentObserver, timingDecision);
    EXPECT_LT(timingDecision, timingGate);
    EXPECT_LT(timingGate, timingUpdate);

    // App-callback draw proof is one-shot: every successful deep submit arms exactly the next callback. Frame
    // timing independently belongs to the deep Present observer whenever it exists, even before topmost draw
    // ownership transfers, so one output cannot be sampled at both the callback and Present boundaries.
    EXPECT_NE(source.find("DX12_ConsumeBelowForeignChainFSRTopmostSubmitProof()", decision - 300),
              std::string::npos);
    EXPECT_NE(ownerQueue.find(
                  "g_BelowForeignChainFSRTopmostSubmitProven.exchange(false, std::memory_order_acq_rel)"),
              std::string::npos);
    EXPECT_NE(ownerQueue.find(
                  "g_BelowForeignChainFSRTopmostSubmitProven.store(true, std::memory_order_release)"),
              std::string::npos);
}

}  // namespace
