#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "hook/d3d12/fsr_screenshot_policy.h"
#include "source_fragment_reader.h"

// Native FSR frame generation with AMD owning the swapchain has no ProcessFrame or PostSL on the presenter, so a screenshot
// request used to wait for the full hook timeout. These tests pin the pieces that service it on the game queue instead:
// the composite must read the application frame and the game's UI resource before the overlay composite writes CE onto
// that UI resource, the overlay must land on the renderer's reserved slot, and nothing in the path may wait on the GPU.

namespace {

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

// Body of one function, from its signature to the first closing brace at column zero.
std::string FunctionBody(const std::string& source, const std::string& signature) {
    const size_t start = source.find(signature);
    if (start == std::string::npos) {
        return {};
    }
    const size_t end = source.find("\n}", start);
    return end == std::string::npos ? source.substr(start) : source.substr(start, end - start);
}

}  // namespace

TEST(FSRScreenshotPolicyTest, ServicedInPreworkBeforeTheOverlayCompositeWritesTheUiResource) {
    const std::string prework =
        FunctionBody(ReadSource("hook/d3d12/dx12_hook_ffx_proxy_present.cpp"), "static void DX12_RunFFXProxyPrePresentWork(");
    ASSERT_FALSE(prework.empty());

    const size_t service = prework.find("DX12_ServiceFSRNoCallbackScreenshotIfPending(proxy);");
    const size_t compositeUi = prework.find("DX12_CompositeOverlayOntoCachedFFXUiResourceOnOwnerQueue(proxy)");
    const size_t compositeBackbuffer = prework.find("DX12_CompositeOverlayOntoSuspendBackbuffer(proxy");
    ASSERT_NE(service, std::string::npos) << "prework must service a pending FSR screenshot";
    ASSERT_NE(compositeUi, std::string::npos);
    ASSERT_NE(compositeBackbuffer, std::string::npos);
    EXPECT_LT(service, compositeUi)
        << "the screenshot copy must precede the overlay draw onto the game's UI resource";
    EXPECT_LT(service, compositeBackbuffer);
}

TEST(FSRScreenshotPolicyTest, OverlayLandsOnTheReservedRendererSlotNotTheBackbufferSlot) {
    const std::string composite = ReadSource("hook/d3d12/dx12_hook_fsr_screenshot.cpp");
    EXPECT_NE(composite.find("overlay.reservedSlot = true;"), std::string::npos);

    const std::string renderer = ReadSource("hook/d3d12/dx12_ffx_suspend_overlay.cpp");
    EXPECT_NE(renderer.find("constexpr size_t kScreenshotSlot = kFrameSlotCount - 1;"), std::string::npos);
    EXPECT_NE(renderer.find("constexpr size_t kOverlayOutputSlotCount = kFrameSlotCount - 1;"), std::string::npos);

    const size_t selectionStart = renderer.find("if (reservedSlot) {");
    const size_t selectionEnd = renderer.find("if (slotIndex == kFrameSlotCount) {", selectionStart);
    ASSERT_NE(selectionStart, std::string::npos);
    ASSERT_NE(selectionEnd, std::string::npos);
    const std::string slotSelection = renderer.substr(selectionStart, selectionEnd - selectionStart);
    const size_t reserved = slotSelection.find("if (reservedSlot) {");
    const size_t inlineBranch = slotSelection.find("} else if (inlineCompletionMarker) {");
    const size_t backbufferBranch = slotSelection.find("} else if (backBufferIndex < kOverlayOutputSlotCount) {");
    ASSERT_NE(reserved, std::string::npos);
    ASSERT_NE(inlineBranch, std::string::npos);
    ASSERT_NE(backbufferBranch, std::string::npos);
    EXPECT_LT(reserved, inlineBranch);
    EXPECT_EQ(slotSelection.find("kFrameSlotCount"), std::string::npos)
        << "inline and backbuffer selection must stay below the reserved screenshot slot";
    EXPECT_NE(slotSelection.find("slotIndex = kScreenshotSlot;"), std::string::npos);
}

TEST(FSRScreenshotPolicyTest, CompositePathNeverWaitsOnTheGpu) {
    const std::string composite = ReadSource("hook/d3d12/dx12_hook_fsr_screenshot.cpp");
    EXPECT_EQ(composite.find("WaitForSingleObject"), std::string::npos);
    EXPECT_EQ(composite.find("SetEventOnCompletion"), std::string::npos);
    EXPECT_EQ(composite.find("->Wait("), std::string::npos);
    EXPECT_EQ(composite.find("Sleep("), std::string::npos);
}

TEST(FSRScreenshotPolicyTest, EverySubmittedCompositeIsRetiredBehindAFence) {
    const std::string composite = ReadSource("hook/d3d12/dx12_hook_fsr_screenshot.cpp");
    EXPECT_NE(composite.find("queue->Signal(retireFence.Get(), ce::fsr_screenshot::GpuCompletion::kFenceValue)"),
              std::string::npos);
    const size_t retain = composite.find("captures.push_back(std::move(prepared));");
    const size_t submit = composite.find("SubmitNativeFSROwnerQueueOverlayCommandList(queue.Get(), list.Get())");
    ASSERT_NE(retain, std::string::npos);
    ASSERT_NE(submit, std::string::npos);
    EXPECT_LT(retain, submit) << "retention storage must exist before submitting GPU work";
    EXPECT_NE(composite.find("prepared.keepAlive = {target, allocator, list, srvHeap, rtvHeap, rootSignature, pipeline,"),
              std::string::npos)
        << "command lists do not retain their referenced resources";
    EXPECT_NE(composite.find("retired.completion.RecordSignal(signalResult);"), std::string::npos);
    EXPECT_NE(composite.find("it->completion.Complete(it->fence->GetCompletedValue())"), std::string::npos);
}

TEST(FSRScreenshotPolicyTest, FailedRetirementSignalRetainsWorkUntilDeviceRemoval) {
    ce::fsr_screenshot::GpuCompletion completion;
    EXPECT_FALSE(completion.Complete(0));
    completion.RecordSignal(E_OUTOFMEMORY);
    for (uint64_t value : {0ull, 1ull, 100ull}) EXPECT_FALSE(completion.Complete(value));
    EXPECT_TRUE(completion.Complete(UINT64_MAX));
}

TEST(FSRScreenshotPolicyTest, SuccessfulRetirementSignalRequiresItsCompletedValue) {
    ce::fsr_screenshot::GpuCompletion completion;
    completion.RecordSignal(S_OK);
    EXPECT_FALSE(completion.Complete(0));
    EXPECT_TRUE(completion.Complete(1));
    EXPECT_TRUE(completion.Complete(UINT64_MAX));
}

TEST(FSRScreenshotPolicyTest, RegisteredUiAlphaConventionReachesTheCompositeConstants) {
    const std::string composite = ReadSource("hook/d3d12/dx12_hook_fsr_screenshot.cpp");
    EXPECT_NE(composite.find("snapshot.flags = g_CachedFFXUiFlags.load"), std::string::npos);
    EXPECT_NE(composite.find("ui.flags & ce::ffx_api::kUiCompositionUsePremulAlpha"), std::string::npos);
    EXPECT_NE(composite.find("sizeof(constants) / sizeof(UINT32), &constants"), std::string::npos);
}

TEST(FSRScreenshotPolicyTest, CompositePostProcessPrecedesOverlayInclusionAndReadback) {
    const std::string composite = ReadSource("hook/d3d12/dx12_hook_fsr_screenshot.cpp");
    const size_t postProcess = composite.find("ce::fsr_screenshot::RecordPostProcess(");
    const size_t includeOverlay = composite.find("if (includeOverlay) {");
    const size_t readback = composite.find("SaveDX12TextureAsScreenshotRaw(");
    ASSERT_NE(postProcess, std::string::npos);
    ASSERT_NE(includeOverlay, std::string::npos);
    ASSERT_NE(readback, std::string::npos);
    EXPECT_LT(postProcess, includeOverlay);
    EXPECT_LT(includeOverlay, readback);
    EXPECT_NE(composite.find("if (postProcessRecorded) ce::sharpen::AbandonRuntimePostProcessCommandList(list.Get());"),
              std::string::npos);
    EXPECT_NE(composite.find("overlay.applyPostProcess = false;"), std::string::npos)
        << "the already corrected screenshot must not be processed a second time with its overlay";
}

TEST(FSRScreenshotPolicyTest, SwapchainReferencesAreNotRetainedPastSubmission) {
    // Holding a swapchain buffer past submission makes the game's ResizeBuffers fail (DXGI_ERROR_INVALID_CALL).
    const std::string composite = ReadSource("hook/d3d12/dx12_hook_fsr_screenshot.cpp");
    EXPECT_EQ(composite.find("pipeline, backBuffer"), std::string::npos);
    EXPECT_EQ(composite.find("pipeline, backBuffer, ui.texture"), std::string::npos);
}

TEST(FSRScreenshotPolicyTest, RetiredCapturesArePrunedOnEveryPresentNotOnlyOnNextRequest) {
    const std::string composite = ReadSource("hook/d3d12/dx12_hook_fsr_screenshot.cpp");
    const size_t service = composite.find("void DX12_ServiceFSRNoCallbackScreenshotIfPending(");
    ASSERT_NE(service, std::string::npos);
    const size_t idleReturn = composite.find("if (requestId == 0 || !proxy) {", service);
    ASSERT_NE(idleReturn, std::string::npos);
    const size_t prune = composite.find("PruneRetiredCaptures();", idleReturn);
    const size_t idleEnd = composite.find("return;", idleReturn);
    ASSERT_NE(prune, std::string::npos);
    EXPECT_LT(prune, idleEnd) << "the no-request path must release finished captures before it returns";
}
