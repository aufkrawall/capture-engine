#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

// Native FSR frame generation with AMD owning the swapchain has no ProcessFrame or PostSL on the presenter, so a screenshot
// request used to wait for the full hook timeout. These tests pin the pieces that service it on the game queue instead:
// the composite must read the application frame and the game's UI resource before the overlay composite writes CE onto
// that UI resource, the overlay must land on the renderer's reserved slot, and nothing in the path may wait on the GPU.

namespace {

std::string ReadSource(const std::filesystem::path& relativePath) {
    std::ifstream file(std::filesystem::current_path() / relativePath, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
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
    EXPECT_NE(composite.find("overlay.screenshotSlot = true;"), std::string::npos);

    const std::string renderer = ReadSource("hook/d3d12/dx12_ffx_suspend_overlay.cpp");
    EXPECT_NE(renderer.find("constexpr size_t kScreenshotSlot = kFrameSlotCount - 1;"), std::string::npos);
    EXPECT_NE(renderer.find("constexpr size_t kOverlayOutputSlotCount = kFrameSlotCount - 1;"), std::string::npos);

    const size_t selectionStart = renderer.find("if (screenshotSlot) {");
    const size_t selectionEnd = renderer.find("if (slotIndex == kFrameSlotCount) {", selectionStart);
    ASSERT_NE(selectionStart, std::string::npos);
    ASSERT_NE(selectionEnd, std::string::npos);
    const std::string slotSelection = renderer.substr(selectionStart, selectionEnd - selectionStart);
    const size_t reserved = slotSelection.find("if (screenshotSlot) {");
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
    EXPECT_NE(composite.find("queue->Signal(retireFence.Get(), 1)"), std::string::npos);
    EXPECT_NE(composite.find("g_retiredCaptures.push_back(std::move(retired));"), std::string::npos);
    EXPECT_NE(composite.find("retired.keepAlive = {target, allocator, list, srvHeap, rtvHeap, rootSignature, pipeline,"),
              std::string::npos)
        << "command lists do not retain their referenced resources";
}
