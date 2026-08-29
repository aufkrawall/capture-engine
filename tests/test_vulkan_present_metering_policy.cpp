#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "../hook/vulkan_layer/vulkan_present_metering_policy.h"

// Regression coverage for `vsync_mode=fifo` behaving like mailbox in Portal RTX
// (RTX Remix), session installed/captureengine/logs/20260829_022419.
//
// CE forced VK_PRESENT_MODE_FIFO_KHR and the driver accepted it, but Remix's
// DLSS 4 multi-frame generation chains VK_NV_present_metering's
// VkSetPresentConfigNV onto every present with numFramesPerBatch=4. The driver
// then paces the batch across one *rendered* frame interval instead of waiting
// for vertical blanks, so the presented rate ran at 165-172/s on a 143 Hz
// display - the tell being that CE's DXGI interception saw the driver switch
// from SyncInterval=1 to SyncInterval=0 plus DXGI_PRESENT_ALLOW_TEARING exactly
// when the frame-generation swapchain went live.

namespace {

using ce::vulkan_present_metering_policy::ChainScan;
using ce::vulkan_present_metering_policy::Decide;
using ce::vulkan_present_metering_policy::Decision;
using ce::vulkan_present_metering_policy::Input;
using ce::vulkan_present_metering_policy::IsVblankPacedPresentMode;
using ce::vulkan_present_metering_policy::kMaxScannedChainNodes;
using ce::vulkan_present_metering_policy::kStructureTypeSetPresentConfigNV;
using ce::vulkan_present_metering_policy::RequestsVblankPacedPresentation;
using ce::vulkan_present_metering_policy::ScanPresentChain;
using ce::vulkan_present_metering_policy::SetPresentConfigNV;

Input MakeInput(VkPresentModeKHR presentMode, uint32_t framesPerBatch, bool chainHead = true) {
    Input input = {};
    input.vblankPacedPresentationRequested = true;
    input.swapchainPresentModeKnown = true;
    input.swapchainPresentMode = presentMode;
    input.swapchainCount = 1;
    input.meteredFramesPerBatch = framesPerBatch;
    input.meteringRequestIsChainHead = chainHead;
    return input;
}

SetPresentConfigNV MakeMeteringNode(uint32_t framesPerBatch, const void* next = nullptr) {
    SetPresentConfigNV node = {};
    node.sType = kStructureTypeSetPresentConfigNV;
    node.pNext = next;
    node.numFramesPerBatch = framesPerBatch;
    node.presentConfigFeedback = 0;
    return node;
}

TEST(VulkanPresentMeteringPolicy, OnlyFifoAndAdaptiveAskForVblankPacing) {
    EXPECT_TRUE(RequestsVblankPacedPresentation("fifo"));
    EXPECT_TRUE(RequestsVblankPacedPresentation("adaptive"));
    EXPECT_FALSE(RequestsVblankPacedPresentation("mailbox"));
    EXPECT_FALSE(RequestsVblankPacedPresentation("off"));
    EXPECT_FALSE(RequestsVblankPacedPresentation("default"));
    EXPECT_FALSE(RequestsVblankPacedPresentation(""));
    // The config layer does not case-normalize vsync_mode, and every existing
    // consumer compares the exact lowercase spellings. Staying identical to
    // them is what keeps the layer's present-mode override and this policy from
    // disagreeing about the same setting.
    EXPECT_FALSE(RequestsVblankPacedPresentation("FIFO"));
}

TEST(VulkanPresentMeteringPolicy, OnlyFifoModesCarryARateContract) {
    EXPECT_TRUE(IsVblankPacedPresentMode(VK_PRESENT_MODE_FIFO_KHR));
    EXPECT_TRUE(IsVblankPacedPresentMode(VK_PRESENT_MODE_FIFO_RELAXED_KHR));
    EXPECT_FALSE(IsVblankPacedPresentMode(VK_PRESENT_MODE_MAILBOX_KHR));
    EXPECT_FALSE(IsVblankPacedPresentMode(VK_PRESENT_MODE_IMMEDIATE_KHR));
}

TEST(VulkanPresentMeteringPolicy, SuppressesMeteringOnAConfiguredFifoSwapchain) {
    // The Portal RTX case: 4x multi-frame generation on a FIFO swapchain.
    const Decision decision = Decide(MakeInput(VK_PRESENT_MODE_FIFO_KHR, 4));
    EXPECT_TRUE(decision.suppressMetering);
    EXPECT_FALSE(decision.blockedByChainPosition);
}

TEST(VulkanPresentMeteringPolicy, SuppressesMeteringUnderAdaptiveFifo) {
    const Decision decision = Decide(MakeInput(VK_PRESENT_MODE_FIFO_RELAXED_KHR, 3));
    EXPECT_TRUE(decision.suppressMetering);
}

TEST(VulkanPresentMeteringPolicy, LeavesMeteringAloneWithoutAFifoSwapchain) {
    EXPECT_FALSE(Decide(MakeInput(VK_PRESENT_MODE_IMMEDIATE_KHR, 4)).suppressMetering);
    EXPECT_FALSE(Decide(MakeInput(VK_PRESENT_MODE_MAILBOX_KHR, 4)).suppressMetering);
}

TEST(VulkanPresentMeteringPolicy, LeavesMeteringAloneWhenTheProfileDidNotAskForFifo) {
    Input input = MakeInput(VK_PRESENT_MODE_FIFO_KHR, 4);
    input.vblankPacedPresentationRequested = false;
    EXPECT_FALSE(Decide(input).suppressMetering);
}

TEST(VulkanPresentMeteringPolicy, LeavesMeteringAloneOnAnUntrackedSwapchain) {
    // Without the swapchain the layer created there is no proof of its pacing
    // contract, and assuming one would be a guess about someone else's chain.
    Input input = MakeInput(VK_PRESENT_MODE_FIFO_KHR, 4);
    input.swapchainPresentModeKnown = false;
    EXPECT_FALSE(Decide(input).suppressMetering);
}

TEST(VulkanPresentMeteringPolicy, SingleFrameBatchIsNotAConflict) {
    // numFramesPerBatch of 0 or 1 asks the driver for nothing FIFO does not
    // already guarantee, so there is nothing to take away.
    EXPECT_FALSE(Decide(MakeInput(VK_PRESENT_MODE_FIFO_KHR, 0)).suppressMetering);
    EXPECT_FALSE(Decide(MakeInput(VK_PRESENT_MODE_FIFO_KHR, 1)).suppressMetering);
}

TEST(VulkanPresentMeteringPolicy, MultiSwapchainPresentIsLeftAlone) {
    Input input = MakeInput(VK_PRESENT_MODE_FIFO_KHR, 4);
    input.swapchainCount = 2;
    EXPECT_FALSE(Decide(input).suppressMetering);
}

TEST(VulkanPresentMeteringPolicy, ADeeperMeteringNodeIsReportedInsteadOfUnlinked) {
    const Decision decision = Decide(MakeInput(VK_PRESENT_MODE_FIFO_KHR, 4, /*chainHead=*/false));
    EXPECT_FALSE(decision.suppressMetering);
    EXPECT_TRUE(decision.blockedByChainPosition);
}

TEST(VulkanPresentMeteringPolicyChainScan, EmptyChainFindsNothing) {
    const ChainScan scan = ScanPresentChain(nullptr);
    EXPECT_FALSE(scan.found);
    EXPECT_EQ(scan.nodeCount, 0u);
    EXPECT_EQ(scan.chainWithoutMetering, nullptr);
    EXPECT_FALSE(scan.truncated);
}

TEST(VulkanPresentMeteringPolicyChainScan, FindsAHeadMeteringNodeAndUnlinksIt) {
    VkPresentIdKHR presentId = {};
    presentId.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
    presentId.pNext = nullptr;
    const SetPresentConfigNV metering = MakeMeteringNode(4, &presentId);

    const ChainScan scan = ScanPresentChain(&metering);
    EXPECT_TRUE(scan.found);
    EXPECT_TRUE(scan.isChainHead);
    EXPECT_EQ(scan.framesPerBatch, 4u);
    EXPECT_EQ(scan.nodeCount, 2u);
    EXPECT_EQ(scan.chainWithoutMetering, static_cast<const void*>(&presentId));
}

TEST(VulkanPresentMeteringPolicyChainScan, ChainWithoutMeteringIsUnchanged) {
    VkPresentIdKHR presentId = {};
    presentId.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;

    const ChainScan scan = ScanPresentChain(&presentId);
    EXPECT_FALSE(scan.found);
    EXPECT_EQ(scan.nodeCount, 1u);
    EXPECT_EQ(scan.chainWithoutMetering, static_cast<const void*>(&presentId));
}

TEST(VulkanPresentMeteringPolicyChainScan, ADeeperMeteringNodeIsFoundButNotUnlinked) {
    const SetPresentConfigNV metering = MakeMeteringNode(3);
    VkPresentIdKHR presentId = {};
    presentId.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
    presentId.pNext = &metering;

    const ChainScan scan = ScanPresentChain(&presentId);
    EXPECT_TRUE(scan.found);
    EXPECT_FALSE(scan.isChainHead);
    EXPECT_EQ(scan.framesPerBatch, 3u);
    // The head is preserved: unlinking a deeper node would mean writing to the
    // application's own const chain.
    EXPECT_EQ(scan.chainWithoutMetering, static_cast<const void*>(&presentId));
}

TEST(VulkanPresentMeteringPolicyChainScan, ASelfReferentialChainTerminates) {
    // A malformed or hostile chain must not spin the present hook.
    VkPresentIdKHR presentId = {};
    presentId.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
    presentId.pNext = &presentId;

    const ChainScan scan = ScanPresentChain(&presentId);
    EXPECT_TRUE(scan.truncated);
    EXPECT_EQ(scan.nodeCount, kMaxScannedChainNodes);
    EXPECT_FALSE(scan.found);
}

}  // namespace
