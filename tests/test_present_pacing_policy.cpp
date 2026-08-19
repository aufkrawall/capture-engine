#include <gtest/gtest.h>

#include <windows.h>

#include <filesystem>
#include <string>

#include "../hook/common/dx12_overlay_policy.h"
#include "../hook/common/dxgi_shared.h"
#include "../hook/common/present_pacing_policy.h"
#include "source_fragment_reader.h"

// Regression coverage for the DOOM Eternal (Vulkan) startup deadlock,
// session installed/captureengine/logs/20260819_020933.
//
// NVIDIA's Vulkan ICD backs VkSwapchainKHR with a DXGI flip swapchain that it
// creates from the game's window thread and presents from a driver-owned
// thread. CE saw that DXGI create, added the frame-latency waitable flag, called
// SetMaximumFrameLatency(1) on it, and then blocked the driver's presenter
// thread in WaitForSingleObject(..., INFINITE) inside Present1. The game
// destroyed the Vulkan swapchain from its window thread, where the ICD joins
// that presenter thread, so the join could never complete; the render thread was
// already blocked in SendMessage to the window thread, and the whole process
// wedged after exactly one presented frame.
//
// CE had already decided the right thing 11 s earlier ("Vulkan layer ownership
// established, skipping D3D/DXGI hooks"). The pacing paths just did not honour
// that decision.

namespace {

using ce::present_pacing_policy::kFlipQueuePacingWaitCeilingMs;
using ce::present_pacing_policy::ShouldApplyCePresentationPolicy;
using ce::present_pacing_policy::ShouldDisablePacingAfterWait;
using ce::present_pacing_policy::ShouldWaitForFlipQueueRoom;
using ce::present_pacing_policy::SlowestHealthyPacingWaitMs;

// RAII restore for the process-global present-path flag.
class ScopedVulkanPresentFlag {
public:
    ScopedVulkanPresentFlag() : previous_(DXGIShared::IsVulkanActive()) {}
    ~ScopedVulkanPresentFlag() { DXGIShared::SetVulkanActiveForDXGIPresentPath(previous_); }

private:
    bool previous_;
};

std::string ReadProjectSource(const char* relativePath) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / relativePath;
    if (!fs::exists(source))
        return {};
    return ce::test_source::ReadFile(source);
}

}  // namespace

TEST(PresentPacingPolicyTest, VulkanLayerOwnedPresentationSuppressesFlipQueuePacing) {
    // The exact DOOM Eternal configuration: backbuffer_count=2 is active and the
    // CE Vulkan layer owns presentation. Waiting here blocks the ICD's presenter
    // thread, which vkDestroySwapchainKHR joins.
    EXPECT_FALSE(ShouldWaitForFlipQueueRoom(/*backbufferCountOverrideActive=*/true,
                                            /*vulkanLayerOwnsPresentation=*/true,
                                            /*pacingLatchedOff=*/false));
    EXPECT_FALSE(ShouldApplyCePresentationPolicy(/*vulkanLayerOwnsPresentation=*/true));
}

TEST(PresentPacingPolicyTest, D3DPresentationStillPaced) {
    // The override must keep working for the games it was written for; the fix
    // may not turn backbuffer_count into a no-op.
    EXPECT_TRUE(ShouldWaitForFlipQueueRoom(/*backbufferCountOverrideActive=*/true,
                                           /*vulkanLayerOwnsPresentation=*/false,
                                           /*pacingLatchedOff=*/false));
    EXPECT_TRUE(ShouldApplyCePresentationPolicy(/*vulkanLayerOwnsPresentation=*/false));
}

TEST(PresentPacingPolicyTest, NoPacingWithoutTheOverride) {
    EXPECT_FALSE(ShouldWaitForFlipQueueRoom(/*backbufferCountOverrideActive=*/false,
                                            /*vulkanLayerOwnsPresentation=*/false,
                                            /*pacingLatchedOff=*/false));
}

TEST(PresentPacingPolicyTest, LatchedOffPacingStaysOff) {
    EXPECT_FALSE(ShouldWaitForFlipQueueRoom(/*backbufferCountOverrideActive=*/true,
                                            /*vulkanLayerOwnsPresentation=*/false,
                                            /*pacingLatchedOff=*/true));
}

TEST(PresentPacingPolicyTest, WaitCeilingIsFiniteAndAboveEveryHealthyWait) {
    EXPECT_NE(kFlipQueuePacingWaitCeilingMs, INFINITE)
        << "an unbounded pacing wait can hold a present thread the graphics runtime joins";

    // The slowest presentation Windows drives is a 24 Hz mode (~42 ms per
    // frame); backbuffer_count accepts at most 6 queued frames.
    const DWORD slowestHealthyWaitMs = SlowestHealthyPacingWaitMs(/*presentationIntervalMs=*/42,
                                                                 /*configuredQueueDepth=*/6);
    EXPECT_TRUE(ce::present_pacing_policy::IsPacingCeilingAboveHealthyWait(kFlipQueuePacingWaitCeilingMs,
                                                                          slowestHealthyWaitMs))
        << "commit ccbdeac5's 16 ms ceiling sat below a healthy wait, so the pacing silently "
           "escaped whenever the game was GPU- or vblank-bound";
    EXPECT_FALSE(ce::present_pacing_policy::IsPacingCeilingAboveHealthyWait(16, slowestHealthyWaitMs));
}

TEST(PresentPacingPolicyTest, MissedCeilingRetiresThePacingSource) {
    EXPECT_TRUE(ShouldDisablePacingAfterWait(WAIT_TIMEOUT));
    EXPECT_TRUE(ShouldDisablePacingAfterWait(WAIT_FAILED));
    EXPECT_TRUE(ShouldDisablePacingAfterWait(WAIT_ABANDONED));
    EXPECT_FALSE(ShouldDisablePacingAfterWait(WAIT_OBJECT_0))
        << "a satisfied wait is the normal case and must keep pacing enabled";
}

TEST(PresentPacingPolicyTest, SharedWaitAcceptsSignalledObjectAndRejectsMissingOne) {
    EXPECT_FALSE(DXGIShared::WaitFlipQueuePacingObject(nullptr, "unit-test"));
    EXPECT_FALSE(DXGIShared::WaitFlipQueuePacingObject(INVALID_HANDLE_VALUE, "unit-test"));

    // A semaphore that already has room behaves exactly like a healthy DXGI
    // frame-latency object: the wait is satisfied without blocking.
    HANDLE room = CreateSemaphoreW(nullptr, 1, 1, nullptr);
    ASSERT_NE(room, nullptr);
    EXPECT_TRUE(DXGIShared::WaitFlipQueuePacingObject(room, "unit-test"));
    CloseHandle(room);
}

TEST(PresentPacingPolicyTest, VulkanWsiCreateKeepsTheRuntimeDescriptor) {
    // Adding the waitable flag behind a graphics runtime's back is the same
    // class of mistake for a Vulkan ICD as it is for an FG runtime.
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldApplySwapchainDescriptorOverridesForCreate(
        /*callerFromThirdPartyOverlay=*/false, /*authoritativeFrameGenerationRuntimeCreator=*/false,
        /*vulkanLayerOwnsPresentation=*/true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldApplySwapchainDescriptorOverridesForCreate(
        /*callerFromThirdPartyOverlay=*/false, /*authoritativeFrameGenerationRuntimeCreator=*/false,
        /*vulkanLayerOwnsPresentation=*/false));
}

TEST(PresentPacingPolicySourceTest, NoUnboundedFlipQueuePacingWaitRemains) {
    // Commit ccbdeac5 fixed this freeze once; commit dd30a5b6 reverted the
    // ceiling to INFINITE and it came back in DOOM Eternal. Assert the shape so
    // a third round cannot land silently.
    const std::string pacing = ReadProjectSource("hook/common/dxgi_shared_present_pacing.cpp");
    ASSERT_FALSE(pacing.empty());
    const size_t wait = pacing.find("WaitForSingleObject(");
    ASSERT_NE(wait, std::string::npos);
    EXPECT_EQ(wait, pacing.rfind("WaitForSingleObject("))
        << "the pacing wait must exist exactly once, in WaitFlipQueuePacingObject";
    EXPECT_NE(pacing.find("kFlipQueuePacingWaitCeilingMs", wait), std::string::npos)
        << "the one pacing wait must carry the shared ceiling";
    EXPECT_EQ(pacing.find(", INFINITE)"), std::string::npos);

    // The two transports that used to carry their own copy must delegate now:
    // one kept an INFINITE wait, the other a 16 ms ceiling that sat below a
    // healthy wait and silently escaped the pacing.
    const std::string original = ReadProjectSource("hook/common/dxgi_shared_original.cpp");
    ASSERT_FALSE(original.empty());
    EXPECT_EQ(original.find("GetFrameLatencyWaitableObject"), std::string::npos)
        << "CallOriginalPresent must pace through WaitBackbufferFrameLatency";
    EXPECT_NE(original.find("WaitBackbufferFrameLatency(pSwapChain)"), std::string::npos);

    const std::string wrapper = ReadProjectSource("hook/wrappers/dxgi_swapchain_wrap_frame_latency.cpp");
    ASSERT_FALSE(wrapper.empty());
    EXPECT_EQ(wrapper.find(", INFINITE)"), std::string::npos);
    const size_t waitFrameLatency = wrapper.find("void CWrapDXGISwapChain::WaitFrameLatency()");
    ASSERT_NE(waitFrameLatency, std::string::npos);
    const size_t waitFrameLatencyEnd = wrapper.find("HANDLE CWrapDXGISwapChain::EnsureFrameLatencyWaitable",
                                                    waitFrameLatency);
    ASSERT_NE(waitFrameLatencyEnd, std::string::npos);
    const std::string waitFrameLatencyBody =
        wrapper.substr(waitFrameLatency, waitFrameLatencyEnd - waitFrameLatency);
    EXPECT_NE(waitFrameLatencyBody.find("DXGIShared::WaitFlipQueuePacingObject"), std::string::npos);
    EXPECT_EQ(waitFrameLatencyBody.find("WaitForSingleObject("), std::string::npos);
}

TEST(PresentPacingPolicySourceTest, PacingHonoursThePublishedVulkanDecision) {
    const std::string pacing = ReadProjectSource("hook/common/dxgi_shared_present_pacing.cpp");
    ASSERT_FALSE(pacing.empty());
    EXPECT_NE(pacing.find("ShouldWaitForFlipQueueRoom"), std::string::npos);
    EXPECT_NE(pacing.find("ShouldApplyCePresentationPolicy"), std::string::npos)
        << "SetMaximumFrameLatency on the Vulkan runtime's transport swapchain is what shrank its "
           "flip queue to a depth the pacing wait could block on";

    const std::string resize = ReadProjectSource("hook/common/dxgi_shared_resize.cpp");
    ASSERT_FALSE(resize.empty());
    EXPECT_NE(resize.find("ShouldApplyCePresentationPolicy"), std::string::npos);
}

TEST(PresentPacingPolicySourceTest, SwapchainCreateFeedsTheVulkanDecisionIntoTheDescriptorRule) {
    const std::string ffxStartup = ReadProjectSource("hook/apis/dx12_hook_ffx_startup.cpp");
    ASSERT_FALSE(ffxStartup.empty());
    const size_t rule = ffxStartup.find("ShouldApplySwapchainDescriptorOverridesForCreate(\n");
    ASSERT_NE(rule, std::string::npos);
    EXPECT_NE(ffxStartup.find("DXGIShared::IsVulkanActive()", rule), std::string::npos)
        << "the create-time descriptor rule must know when the CE Vulkan layer owns presentation";
}

TEST(PresentPacingPolicyTest, PublishedVulkanDecisionDrivesTheRuntimePredicate) {
    ScopedVulkanPresentFlag restore;
    DXGIShared::SetVulkanActiveForDXGIPresentPath(false);
    EXPECT_TRUE(ShouldApplyCePresentationPolicy(DXGIShared::IsVulkanActive()));
    DXGIShared::SetVulkanActiveForDXGIPresentPath(true);
    EXPECT_FALSE(ShouldApplyCePresentationPolicy(DXGIShared::IsVulkanActive()));
}
