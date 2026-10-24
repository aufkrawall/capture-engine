#include <gtest/gtest.h>

#include <filesystem>
#include <limits>
#include <string>

#include "hook/d3d12/dx12_overlay_policy/ffx_substitute_retirement.h"
#include "hook/d3d12/dx12_overlay_policy/ffx_topmost_batch.h"

#include "source_fragment_reader.h"

namespace {

using ce::dx12_overlay_policy::AdvanceFinalECLBatchSignatureStability;
using ce::dx12_overlay_policy::ChooseTopmostAnchorBatch;
using ce::dx12_overlay_policy::FinalECLBatchSignature;
using ce::dx12_overlay_policy::HasCompletedNoCallbackTopmostActivation;
using ce::dx12_overlay_policy::IsNewTopmostPresentation;
using ce::dx12_overlay_policy::IsValidFinalECLBatchSignature;
using ce::dx12_overlay_policy::PresenterBatch;
using ce::dx12_overlay_policy::SameFinalECLBatchSignature;
using ce::dx12_overlay_policy::ShouldAppendTopmostOverlayToFinalECLBatch;
using ce::dx12_overlay_policy::ShouldGrantNoCallbackTopmostOwnership;
using ce::dx12_overlay_policy::ShouldRenderAppCallbackTopmostOverlay;
using ce::dx12_overlay_policy::ShouldRetireWarmFSRRendererForPresentationChange;
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
    EXPECT_FALSE(HasCompletedNoCallbackTopmostActivation(false, true, true, true));
    EXPECT_FALSE(HasCompletedNoCallbackTopmostActivation(true, false, true, true));
    EXPECT_FALSE(HasCompletedNoCallbackTopmostActivation(true, true, false, true));
    EXPECT_TRUE(HasCompletedNoCallbackTopmostActivation(true, true, true, true));

    EXPECT_FALSE(ShouldGrantNoCallbackTopmostOwnership(false, true));
    EXPECT_FALSE(ShouldGrantNoCallbackTopmostOwnership(true, false));
    EXPECT_TRUE(ShouldGrantNoCallbackTopmostOwnership(true, true));
}

// Session 20261024_015207: AMD presented the first outputs after FG on from the game thread; the route proven there
// owned frames whose outputs AMD's presenter thread then composed without CE's draw.
TEST(FFXTopmostBatchPolicyTest, OwnershipWaitsForTheRuntimesOwnPresenterThread) {
    EXPECT_FALSE(HasCompletedNoCallbackTopmostActivation(true, true, true, /*presentedOffTheGameThread=*/false));
    // A new presenting thread relearns the route like a new swapchain or queue.
    EXPECT_TRUE(IsNewTopmostPresentation(false, false, /*presenterThreadChanged=*/true));
    EXPECT_TRUE(IsNewTopmostPresentation(true, false, false));
    EXPECT_TRUE(IsNewTopmostPresentation(false, true, false));
    EXPECT_FALSE(IsNewTopmostPresentation(false, false, false));
}

// Session 20261024_015207: Steam's and OptiScaler's start-up messages submitted after the runtime and stopped
// mid-run; anchoring on them left 7 outputs without the overlay.
TEST(FFXTopmostBatchPolicyTest, AnchorIsTheRuntimesLastBatchNotAPresentChainOverlay) {
    const PresenterBatch withOverlays[] = {
        {0x1000, 0xA0, true},   // the runtime composes the output
        {0x1000, 0xA0, true},   // and submits a second time from the same site
        {0x5000, 0xA0, false},  // a Present-chain overlay
        {0x6000, 0xA0, false},  // another one
    };
    const FinalECLBatchSignature anchor = ChooseTopmostAnchorBatch(withOverlays, 4, true);
    EXPECT_EQ(anchor.callSite, 0x1000u);
    EXPECT_EQ(anchor.queueIdentity, 0xA0u);
    EXPECT_EQ(anchor.ordinal, 2u);
    // The same Present without those overlays keeps the same anchor: their start and stop no longer move the route.
    const FinalECLBatchSignature alone = ChooseTopmostAnchorBatch(withOverlays, 2, true);
    EXPECT_TRUE(SameFinalECLBatchSignature(anchor, alone));
    EXPECT_EQ(AdvanceFinalECLBatchSignatureStability(anchor, 2, alone), 3u);
}

TEST(FFXTopmostBatchPolicyTest, AnchorNeedsARuntimeBatchAndFallsBackToTheLastBatchWithoutAnImage) {
    const PresenterBatch foreignOnly[] = {{0x5000, 0xA0, false}, {0x6000, 0xA0, false}};
    EXPECT_FALSE(IsValidFinalECLBatchSignature(ChooseTopmostAnchorBatch(foreignOnly, 2, true)));
    const FinalECLBatchSignature last = ChooseTopmostAnchorBatch(foreignOnly, 2, /*runtimeImageKnown=*/false);
    EXPECT_EQ(last.callSite, 0x6000u);
    EXPECT_EQ(last.ordinal, 1u);
    EXPECT_FALSE(IsValidFinalECLBatchSignature(ChooseTopmostAnchorBatch(nullptr, 0, true)));
}

TEST(FFXTopmostBatchPolicyTest, AppCallbackTopmostRouteStartsWithNonVisibleProbe) {
    EXPECT_FALSE(ShouldRenderAppCallbackTopmostOverlay(/*routeArmed=*/false));
    EXPECT_TRUE(ShouldRenderAppCallbackTopmostOverlay(/*routeArmed=*/true));
}

TEST(FFXTopmostBatchPolicyTest, TemporaryRoutingChangesKeepWarmRendererResources) {
    EXPECT_FALSE(ShouldRetireWarmFSRRendererForPresentationChange(
        /*hadPresentation=*/true, /*hasReplacementPresentation=*/false,
        /*presentationIdentityChanged=*/true));
    EXPECT_FALSE(ShouldRetireWarmFSRRendererForPresentationChange(
        /*hadPresentation=*/false, /*hasReplacementPresentation=*/true,
        /*presentationIdentityChanged=*/true));
    EXPECT_FALSE(ShouldRetireWarmFSRRendererForPresentationChange(
        /*hadPresentation=*/true, /*hasReplacementPresentation=*/true,
        /*presentationIdentityChanged=*/false));
    EXPECT_TRUE(ShouldRetireWarmFSRRendererForPresentationChange(
        /*hadPresentation=*/true, /*hasReplacementPresentation=*/true,
        /*presentationIdentityChanged=*/true));
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

TEST(FFXTopmostBatchSourceTest, OverlayAndCaptureShareOneExistingExecuteCommandListsCall) {
    const std::string source = ReadSource("hook/d3d12/dx12_hook_ffx_topmost_batch.cpp");
    const std::string capture = ReadSource("hook/d3d12/dx12_hook_fsr_capture.cpp");
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(capture.empty());
    EXPECT_NE(source.find("DX12_SubmitFSRBatchWithCapture("), std::string::npos);
    const size_t before = capture.find("if (copy && !batch.includeOverlay)");
    const size_t overlay = capture.find("batch.lists[batch.count++] = batch.overlay;");
    const size_t after = capture.find("if (copy && batch.includeOverlay)");
    const size_t submit = capture.find("batch.original(queue, batch.count, batch.lists.data());");
    ASSERT_NE(before, std::string::npos);
    ASSERT_NE(overlay, std::string::npos);
    ASSERT_NE(after, std::string::npos);
    ASSERT_NE(submit, std::string::npos);
    EXPECT_LT(before, overlay);
    EXPECT_LT(overlay, after);
    EXPECT_LT(after, submit);
    EXPECT_EQ(capture.find("batch.original(queue,", submit + 1), std::string::npos);
    EXPECT_NE(source.find("request.inlineCompletionMarker = true;"), std::string::npos);
    EXPECT_EQ(source.find("Signal("), std::string::npos);
    EXPECT_EQ(capture.find("Signal("), std::string::npos);
}

TEST(FFXTopmostBatchSourceTest, HotPathIsGenericAndExcludesCEOwnedSubmissions) {
    const std::string source = ReadSource("hook/d3d12/dx12_hook_ecl.cpp");
    ASSERT_FALSE(source.empty());

    const size_t exclusion = source.find("dx12_hook_s_insideCEOverlayECLDepth == 0");
    const size_t append = source.find("DX12_TryAppendNoCallbackFSRTopmostOverlayToECL", exclusion);
    ASSERT_NE(exclusion, std::string::npos);
    ASSERT_NE(append, std::string::npos);
    EXPECT_LT(exclusion, append);
}

TEST(FFXTopmostBatchSourceTest, FallbackAndInlineRenderersCoexistAndHandoffWithoutDoubleBlend) {
    const std::string renderer = ReadSource("hook/d3d12/dx12_ffx_suspend_overlay.cpp");
    const std::string proxy = ReadSource("hook/d3d12/dx12_hook_ffx_proxy_present.cpp");
    const std::string topmostBatch = ReadSource("hook/d3d12/dx12_hook_ffx_topmost_batch.cpp");
    ASSERT_FALSE(renderer.empty());
    ASSERT_FALSE(proxy.empty());
    ASSERT_FALSE(topmostBatch.empty());

    EXPECT_NE(renderer.find("request.inlineCompletionMarker ? g_InlineProxyStates : g_ProxyStates"),
              std::string::npos);
    EXPECT_NE(renderer.find("WriteBufferImmediate"), std::string::npos);
    EXPECT_NE(renderer.find("const bool writesTarget = clearTransparent || renderOverlay;"),
              std::string::npos);
    // Inline candidates stop short of the reserved FSR screenshot slot, so a marker-proven overlay output never
    // reuses allocator or upload state a screenshot render still owns.
    EXPECT_NE(renderer.find("const size_t candidate = (firstCandidate + offset) % kOverlayOutputSlotCount;"),
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
    const std::string ownerQueue = ReadSource("hook/d3d12/dx12_hook_ffx_owner_queue.cpp");
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

TEST(FFXTopmostBatchSourceTest, AppCallbackTopmostRouteRunsBeforeNormalBackendEarlyReturns) {
    const std::string processSession = ReadSource("hook/d3d12/dx12_hook_process_session.cpp");
    const std::string phase3Source = ReadSource("hook/d3d12/dx12_hook_process_session_stage3_overlay_init.cpp");
    const std::string phase5 = ReadSource("hook/d3d12/dx12_hook_process_session_stage5_fg_transition.cpp");
    const std::string drawMain = ReadSource("hook/d3d12/dx12_hook_process_session_draw_main.cpp");
    ASSERT_FALSE(processSession.empty());
    ASSERT_FALSE(phase3Source.empty());
    ASSERT_FALSE(phase5.empty());
    ASSERT_FALSE(drawMain.empty());

    const size_t phase2 = processSession.find("flow = TrackSwapchainAndSelectQueue();");
    const size_t independentRoute =
        processSession.find("TryCompositeOverlayBelowForeignChainForRuntimeOwnedFSR()", phase2);
    const size_t phase3 = processSession.find("flow = InitOverlayBackend();", independentRoute);
    ASSERT_NE(phase2, std::string::npos);
    ASSERT_NE(independentRoute, std::string::npos);
    ASSERT_NE(phase3, std::string::npos);
    EXPECT_LT(phase2, independentRoute);
    EXPECT_LT(independentRoute, phase3)
        << "normal-backend InitOverlayBackend returns after the FSR cooldown and must not preempt the independent route";
    const size_t runtimeOwnedSkip =
        phase3Source.find("ShouldSkipSeparateOverlayGpuWorkForCurrentSwapchain(");
    const size_t runtimeOwnedFlow = phase3Source.find("return ProcessFrameFlow::", runtimeOwnedSkip);
    ASSERT_NE(runtimeOwnedSkip, std::string::npos);
    ASSERT_NE(runtimeOwnedFlow, std::string::npos);
    EXPECT_EQ(phase3Source.find("return ProcessFrameFlow::kSkipOverlayInit;", runtimeOwnedSkip), runtimeOwnedFlow)
        << "InitOverlayBackend's runtime-owned FSR deferral skips only the normal-backend init";

    const size_t drawFrame = phase5.find("ProcessFrameFlow FrameProcessSession::DrawOverlayFrame()");
    ASSERT_NE(drawFrame, std::string::npos);
    EXPECT_EQ(phase5.find("TryCompositeOverlayBelowForeignChainForRuntimeOwnedFSR()", drawFrame), std::string::npos);
    EXPECT_NE(phase5.find("independentFSRTopmostCompositedThisPresent", drawFrame), std::string::npos);
    const size_t oldNestedSite = drawMain.find("ProcessFrameFlow FrameProcessSession::DrawSkipAndCounters()");
    const size_t helperDefinition =
        drawMain.find("bool FrameProcessSession::TryCompositeOverlayBelowForeignChainForRuntimeOwnedFSR()", oldNestedSite);
    ASSERT_NE(oldNestedSite, std::string::npos);
    ASSERT_NE(helperDefinition, std::string::npos);
    const std::string oldNestedBody = drawMain.substr(oldNestedSite, helperDefinition - oldNestedSite);
    EXPECT_EQ(oldNestedBody.find("TryCompositeOverlayBelowForeignChainForRuntimeOwnedFSR()"), std::string::npos)
        << "the independent renderer must not be re-nested behind the normal backend skip gate";
}

TEST(FFXTopmostBatchSourceTest, AppCallbackRouteReusesWarmMarkerRendererAndPinsPresentationQueue) {
    const std::string callbackAdapter = ReadSource("hook/d3d12/dx12_hook_ffx_overlay_adapter.cpp");
    const std::string ownerQueue = ReadSource("hook/d3d12/dx12_hook_ffx_owner_queue.cpp");
    const std::string topmostBatch = ReadSource("hook/d3d12/dx12_hook_ffx_topmost_batch.cpp");
    const std::string renderer = ReadSource("hook/d3d12/dx12_ffx_suspend_overlay.cpp");
    const std::string drawMain = ReadSource("hook/d3d12/dx12_hook_process_session_draw_main.cpp");
    ASSERT_FALSE(callbackAdapter.empty());
    ASSERT_FALSE(ownerQueue.empty());
    ASSERT_FALSE(topmostBatch.empty());
    ASSERT_FALSE(renderer.empty());
    ASSERT_FALSE(drawMain.empty());

    const size_t deepComposite = ownerQueue.find(
        "bool DX12_CompositeOverlayBelowForeignChainForRuntimeOwnedFSR(");
    ASSERT_NE(deepComposite, std::string::npos);
    const std::string deepCompositeBody = ownerQueue.substr(deepComposite);
    const size_t helper =
        drawMain.find("bool FrameProcessSession::TryCompositeOverlayBelowForeignChainForRuntimeOwnedFSR()");
    const size_t nextFunction = drawMain.find("ProcessFrameFlow FrameProcessSession::ExecuteDrawTransaction()", helper);
    ASSERT_NE(helper, std::string::npos);
    ASSERT_NE(nextFunction, std::string::npos);
    const std::string helperBody = drawMain.substr(helper, nextFunction - helper);

    EXPECT_NE(deepCompositeBody.find("request.inlineCompletionMarker = true;"), std::string::npos);
    EXPECT_EQ(deepCompositeBody.find("request.signalFence = &SignalNativeFSROwnerQueueOverlayFence;"),
              std::string::npos);
    EXPECT_NE(topmostBatch.find("request.embeddedInExistingBatch = true;"), std::string::npos);
    EXPECT_NE(renderer.find("request.inlineCompletionMarker ? g_InlineProxyStates : g_ProxyStates"),
              std::string::npos);
    EXPECT_NE(topmostBatch.find(
                  "DX12_PrewarmFFXPresentCallbackOverlayAdapter(g_TopmostBatchSwapChain, queue)"),
              std::string::npos);
    EXPECT_NE(callbackAdapter.find("\"no-callback topmost prewarm\", false"), std::string::npos);
    EXPECT_NE(callbackAdapter.find("\"live app-callback\", true"), std::string::npos);

    EXPECT_NE(helperBody.find("presentationQueue = dx12_hook_g_SwapchainQueue;"), std::string::npos);
    EXPECT_NE(helperBody.find("presentationQueue->AddRef();"), std::string::npos);
    EXPECT_NE(helperBody.find(
                  "DX12_CompositeOverlayBelowForeignChainForRuntimeOwnedFSR(pSwapChain, presentationQueue)"),
              std::string::npos);
    EXPECT_NE(helperBody.find("presentationQueue->Release();"), std::string::npos);
    EXPECT_EQ(helperBody.find(
                  "DX12_CompositeOverlayBelowForeignChainForRuntimeOwnedFSR(pSwapChain, gameQueue)"),
              std::string::npos);
}

TEST(FFXTopmostBatchSourceTest, HiddenCallbackHasNoDiagnosticGpuTailAndStillPublishesTiming) {
    const std::string source = ReadSource("hook/d3d12/dx12_hook_ffx.cpp");
    const auto gate = source.find("if (overlayDrawn || shouldComposeCurrentToOutput) {");
    ASSERT_NE(gate, std::string::npos);
    const auto end = source.find("\n    }", gate);
    ASSERT_NE(end, std::string::npos);
    const auto tail = source.substr(gate, end - gate);
    EXPECT_NE(tail.find("kOverlayBcAfterDraw"), std::string::npos);
    EXPECT_NE(tail.find("kOverlayBcBeforeClose"), std::string::npos);
    EXPECT_EQ(source.find("WriteOverlayGpuBreadcrumb(", end), std::string::npos);
    EXPECT_NE(source.find("NotifyOverlayCallbackDraw(", end), std::string::npos);
    EXPECT_NE(source.find("DX12_UpdateFFXPresentCallbackFrameTiming(", end), std::string::npos);
    EXPECT_NE(source.find("[FSRCallbackWork]"), std::string::npos);
}

TEST(FFXTopmostBatchSourceTest, CallbackBridgeCacheInvalidatesAtShutdownAndAvoidsSteadyMutex) {
    const std::string callback = ReadSource("hook/d3d12/dx12_hook_ffx.cpp");
    const std::string registry = ReadSource("hook/d3d12/dx12_hook_ffx_callback_bridge.cpp");
    const std::string shutdown = ReadSource("hook/d3d12/dx12_hook_main.cpp");
    EXPECT_EQ(callback.find("lock(dx12_hook_g_FFXPresentCallbackBridgeMutex)"), std::string::npos);
    EXPECT_NE(callback.find("DX12_ResolveFFXPresentCallbackBridge(userCtx)"), std::string::npos);
    EXPECT_NE(shutdown.find("DX12_ClearAllFFXPresentCallbackBridges();"), std::string::npos);
    const auto clear = registry.find("void DX12_ClearAllFFXPresentCallbackBridges()");
    ASSERT_NE(clear, std::string::npos);
    const auto end = registry.find("\n}", clear);
    EXPECT_NE(registry.substr(clear, end - clear).find("s_bridgeGeneration.fetch_add("), std::string::npos);
}

TEST(FFXTopmostBatchSourceTest, RoutingEdgesRetainWarmStateAndHotDiagnosticsAreStateful) {
    const std::string topmostBatch = ReadSource("hook/d3d12/dx12_hook_ffx_topmost_batch.cpp");
    const std::string renderer = ReadSource("hook/d3d12/dx12_ffx_suspend_overlay.cpp");
    const std::string ownerQueue = ReadSource("hook/d3d12/dx12_hook_ffx_owner_queue.cpp");
    const std::string layerLogging = ReadSource("hook/present/dxgi_shared_hooks.cpp");
    const std::string ffx = ReadSource("hook/d3d12/dx12_hook_ffx.cpp");
    ASSERT_FALSE(topmostBatch.empty());
    ASSERT_FALSE(renderer.empty());
    ASSERT_FALSE(ownerQueue.empty());
    ASSERT_FALSE(layerLogging.empty());
    ASSERT_FALSE(ffx.empty());

    const size_t replacementPolicy = topmostBatch.find(
        "ShouldRetireWarmFSRRendererForPresentationChange(");
    const size_t guardedRetirement = topmostBatch.find("if (presentationReplaced) {", replacementPolicy);
    const size_t retirement = topmostBatch.find("RetireProxy(oldSwapChain", guardedRetirement);
    const size_t preservedLog = topmostBatch.find("preserving its warm renderer", retirement);
    ASSERT_NE(replacementPolicy, std::string::npos);
    ASSERT_NE(guardedRetirement, std::string::npos);
    ASSERT_NE(retirement, std::string::npos);
    ASSERT_NE(preservedLog, std::string::npos);
    EXPECT_LT(replacementPolicy, guardedRetirement);
    EXPECT_LT(guardedRetirement, retirement);
    EXPECT_LT(retirement, preservedLog);
    EXPECT_NE(renderer.find("ComPtr<IDXGISwapChain> proxyLifetime;"), std::string::npos);
    EXPECT_NE(renderer.find("proxyLifetime = newProxy;"), std::string::npos);
    EXPECT_NE(renderer.find("state->ReleaseProxyLifetime();"), std::string::npos);
    EXPECT_NE(ownerQueue.find("RetireAllForNativeFSRTeardown(reason);"), std::string::npos);

    EXPECT_NE(layerLogging.find("s_loggedSites.fetch_or(siteBit"), std::string::npos);
    EXPECT_EQ(layerLogging.find("s_currentSite.exchange"), std::string::npos);
    EXPECT_NE(ffx.find("DX12_LogRuntimeOwnedCallbackHDRSourceChange("), std::string::npos);
    EXPECT_EQ(ffx.find("\"DX12: Cached runtime-owned callback HDR source\""), std::string::npos);
}

// The no-callback route retires for the frames after the change; the frames AMD still composes keep it.
TEST(FFXTopmostBatchSourceTest, CallbackRoutingChangeRetiresBothTopmostRoutes) {
    const std::string source = ReadSource("hook/d3d12/dx12_hook_ffx.cpp");
    ASSERT_FALSE(source.empty());

    const size_t configured = source.find("void DX12_OnNativeFSRPresentCallbackRoutingConfigured(");
    const size_t appRouteReset = source.find("DX12_ResetBelowForeignChainFSRTopmostSubmitProof(", configured);
    const size_t noCallbackRouteReset =
        source.find("DX12_ClearNoCallbackFSRTopmostBatchAfterComposedFrames(", configured);
    const size_t stateWrite = source.find("dx12_hook_g_FFXPresentCallbackBridgeExpected.store(", configured);
    ASSERT_NE(configured, std::string::npos);
    ASSERT_NE(appRouteReset, std::string::npos);
    ASSERT_NE(noCallbackRouteReset, std::string::npos);
    ASSERT_NE(stateWrite, std::string::npos);
    EXPECT_LT(appRouteReset, stateWrite);
    EXPECT_LT(noCallbackRouteReset, stateWrite);
}

TEST(FFXTopmostBatchSourceTest, CallbackDrawAndFrameTimingUseIndependentExactOwners) {
    const std::string source = ReadSource("hook/d3d12/dx12_hook_ffx.cpp");
    const std::string metrics = ReadSource("hook/d3d12/dx12_hook_ffx_metrics.cpp");
    const std::string ownerQueue = ReadSource("hook/d3d12/dx12_hook_ffx_owner_queue.cpp");
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(metrics.empty());
    ASSERT_FALSE(ownerQueue.empty());

    const size_t decision = source.find("const bool callbackYieldsToTopmostRoute =");
    // Matched in two parts so a diagnostic conjunct (the frame-generation cost probe) can sit
    // between them: what this pins is that the draw is gated on the yield decision and precedes
    // the timing update, not the exact spelling of the condition.
    const size_t drawGate = source.find("if (!callbackYieldsToTopmostRoute &&", decision);
    const size_t draw = source.find("RenderOverlayViaFFXPresentCallback(desc))", drawGate);
    const size_t timingCall = source.find("DX12_UpdateFFXPresentCallbackFrameTiming(", draw);
    const size_t deepPresentObserver = metrics.find("DXGIShared::IsPresentInterceptedBelowForeignChain()");
    const size_t timingDecision = metrics.find("ShouldSampleFrameTimingFromFFXPresentCallback(", deepPresentObserver);
    const size_t timingGate = metrics.find("if (callbackSamplesFrameTiming) {", timingDecision);
    const size_t timingUpdate = metrics.find("metrics->Update(PerfLogger::GetQpcUs());", timingGate);
    ASSERT_NE(decision, std::string::npos);
    ASSERT_NE(drawGate, std::string::npos);
    ASSERT_NE(draw, std::string::npos);
    ASSERT_NE(timingCall, std::string::npos);
    ASSERT_NE(deepPresentObserver, std::string::npos);
    ASSERT_NE(timingDecision, std::string::npos);
    ASSERT_NE(timingGate, std::string::npos);
    ASSERT_NE(timingUpdate, std::string::npos);
    EXPECT_LT(decision, drawGate);
    EXPECT_LT(drawGate, draw);
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

// GTA V Enhanced's Alt+F4 quit dialog suspends FSR FG (session 20261018_210920):
// AMD's no-interpolation present still blends the registered UI resource over
// the proxy backbuffer, and CE's substitute kept its last overlay frame - a
// frozen ghost above the live overlay CE drew on the backbuffer.
TEST(FFXSubstituteRetirementPolicyTest, CESubstituteIsClearedOncePerProxyBackbufferRouteEntry) {
    using ce::dx12_overlay_policy::ShouldRetireCESubstituteBeforeProxyBackbufferRoute;
    EXPECT_TRUE(ShouldRetireCESubstituteBeforeProxyBackbufferRoute(true, true, false));
    EXPECT_FALSE(ShouldRetireCESubstituteBeforeProxyBackbufferRoute(true, true, true)) << "once per entry";
    EXPECT_FALSE(ShouldRetireCESubstituteBeforeProxyBackbufferRoute(true, false, false))
        << "a game-owned UI texture is rewritten by its producer";
    EXPECT_FALSE(ShouldRetireCESubstituteBeforeProxyBackbufferRoute(false, true, false))
        << "the UI-resource route draws the substitute itself";
}

TEST(FFXSubstituteRetirementPolicyTest, ProxyPreworkClearsTheSubstituteBeforeDrawingOnTheBackbuffer) {
    const std::string source = ReadSource("hook/d3d12/dx12_hook_ffx_proxy_present.cpp");
    const size_t decision = source.find("ShouldRetireCESubstituteBeforeProxyBackbufferRoute(");
    const size_t clear =
        source.find("DX12_CompositeOverlayOntoCachedFFXUiResourceOnOwnerQueue(proxy, /*clearOnly=*/true)", decision);
    const size_t backbuffer = source.find("DX12_CompositeOverlayOntoSuspendBackbuffer(proxy", decision);
    ASSERT_NE(decision, std::string::npos);
    ASSERT_NE(clear, std::string::npos);
    ASSERT_NE(backbuffer, std::string::npos);
    EXPECT_LT(clear, backbuffer);
}
