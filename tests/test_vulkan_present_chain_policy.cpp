#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "../hook/vulkan_layer/vulkan_present_chain_policy.h"

// Portal RTX sessions `20260830_175147` and `20260830_182939`: CE overrode the
// swapchain's present mode to FIFO, the driver reported `presentMode=2`, and
// the very same swapchain was then presented through DXGI with
// `SyncInterval=0` plus `DXGI_PRESENT_ALLOW_TEARING` - while a swapchain
// generation the application itself created as FIFO, in the same process, used
// `SyncInterval=1`. A per-present mode selection is what that looks like from
// below, so the layer forces one back to the mode the swapchain was created
// with and prints the chain either way.

namespace {

using ce::vulkan_present_chain::ChainDescription;
using ce::vulkan_present_chain::DecidePresentModeSelection;
using ce::vulkan_present_chain::DescribeChain;
using ce::vulkan_present_chain::kMaxScannedChainNodes;
using ce::vulkan_present_chain::kStructureTypeSwapchainPresentModeInfo;
using ce::vulkan_present_chain::PresentModeSelectionDecision;
using ce::vulkan_present_chain::PresentModeSelectionInput;
using ce::vulkan_present_chain::SwapchainPresentModeInfo;

SwapchainPresentModeInfo MakeSelection(const VkPresentModeKHR* modes, uint32_t count, const void* next = nullptr) {
    SwapchainPresentModeInfo node = {};
    node.sType = kStructureTypeSwapchainPresentModeInfo;
    node.pNext = next;
    node.swapchainCount = count;
    node.pPresentModes = modes;
    return node;
}

PresentModeSelectionInput MakeInput(VkPresentModeKHR created, VkPresentModeKHR selected, bool chainHead = true) {
    PresentModeSelectionInput input = {};
    input.vblankPacedPresentationRequested = true;
    input.swapchainPresentModeKnown = true;
    input.swapchainPresentMode = created;
    input.presentSwapchainCount = 1;
    input.hasPresentModeSelection = true;
    input.presentModeSelectionIsChainHead = chainHead;
    input.presentModeSelectionSwapchainCount = 1;
    input.selectedPresentMode = selected;
    return input;
}

TEST(VulkanPresentChainPolicy, EmptyChainIsDescribedRatherThanSkipped) {
    char text[64] = {};
    const ChainDescription description = DescribeChain(nullptr, text, sizeof(text));
    EXPECT_EQ(description.nodeCount, 0u);
    EXPECT_FALSE(description.hasPresentModeSelection);
    EXPECT_FALSE(description.truncated);
    EXPECT_STREQ(text, "<empty>");
}

TEST(VulkanPresentChainPolicy, NamesTheNodesThatCanChangePresentation) {
    VkPresentIdKHR presentId = {};
    presentId.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
    const VkPresentModeKHR selected = VK_PRESENT_MODE_IMMEDIATE_KHR;
    const SwapchainPresentModeInfo selection = MakeSelection(&selected, 1, &presentId);

    char text[128] = {};
    const ChainDescription description = DescribeChain(&selection, text, sizeof(text));
    EXPECT_EQ(description.nodeCount, 2u);
    EXPECT_TRUE(description.hasPresentModeSelection);
    EXPECT_TRUE(description.presentModeSelectionIsChainHead);
    EXPECT_EQ(description.selectedPresentMode, VK_PRESENT_MODE_IMMEDIATE_KHR);
    EXPECT_EQ(description.chainWithoutPresentModeSelection, static_cast<const void*>(&presentId));
    EXPECT_STREQ(text, "SwapchainPresentModeInfo(mode=0) -> PresentId");
}

TEST(VulkanPresentChainPolicy, AnUnknownNodeIsReportedByNumber) {
    VkBaseInStructure unknown = {};
    unknown.sType = static_cast<VkStructureType>(4242);
    char text[64] = {};
    const ChainDescription description = DescribeChain(&unknown, text, sizeof(text));
    EXPECT_EQ(description.nodeCount, 1u);
    EXPECT_STREQ(text, "sType=4242");
}

TEST(VulkanPresentChainPolicy, ASelfReferentialChainTerminates) {
    VkPresentIdKHR presentId = {};
    presentId.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
    presentId.pNext = &presentId;
    char text[512] = {};
    const ChainDescription description = DescribeChain(&presentId, text, sizeof(text));
    EXPECT_TRUE(description.truncated);
    EXPECT_EQ(description.nodeCount, kMaxScannedChainNodes);
}

TEST(VulkanPresentChainPolicy, DescriptionNeverOverrunsTheCallersBuffer) {
    VkPresentIdKHR presentId = {};
    presentId.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
    presentId.pNext = &presentId;
    char text[8] = {};
    text[7] = '\0';
    DescribeChain(&presentId, text, sizeof(text));
    EXPECT_EQ(std::strlen(text), 7u);
}

TEST(VulkanPresentChainPolicy, ForcesAPerPresentImmediateBackToTheCreatedFifoMode) {
    const PresentModeSelectionDecision decision =
        DecidePresentModeSelection(MakeInput(VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR));
    EXPECT_TRUE(decision.forceCreatedMode);
    EXPECT_FALSE(decision.blockedByChainPosition);
}

TEST(VulkanPresentChainPolicy, ForcesUnderAdaptiveFifoToo) {
    EXPECT_TRUE(DecidePresentModeSelection(MakeInput(VK_PRESENT_MODE_FIFO_RELAXED_KHR, VK_PRESENT_MODE_MAILBOX_KHR))
                    .forceCreatedMode);
}

TEST(VulkanPresentChainPolicy, LeavesASelectionThatAlreadyMatchesAlone) {
    EXPECT_FALSE(DecidePresentModeSelection(MakeInput(VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_KHR))
                     .forceCreatedMode);
}

TEST(VulkanPresentChainPolicy, NeverForcesOnASwapchainWithNoRateContract) {
    // The forced value is always the swapchain's own creation mode, so a
    // swapchain CE did not put a rate contract on has nothing to defend.
    EXPECT_FALSE(DecidePresentModeSelection(MakeInput(VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR))
                     .forceCreatedMode);
    EXPECT_FALSE(DecidePresentModeSelection(MakeInput(VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR))
                     .forceCreatedMode);
}

TEST(VulkanPresentChainPolicy, LeavesTheSelectionAloneWhenTheProfileDidNotAskForPacing) {
    PresentModeSelectionInput input = MakeInput(VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR);
    input.vblankPacedPresentationRequested = false;
    EXPECT_FALSE(DecidePresentModeSelection(input).forceCreatedMode);
}

TEST(VulkanPresentChainPolicy, LeavesTheSelectionAloneOnAnUntrackedSwapchain) {
    PresentModeSelectionInput input = MakeInput(VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR);
    input.swapchainPresentModeKnown = false;
    EXPECT_FALSE(DecidePresentModeSelection(input).forceCreatedMode);
}

TEST(VulkanPresentChainPolicy, RefusesToRewriteAMultiSwapchainSelection) {
    // CE writes a one-entry array, so a selection covering several swapchains
    // is not one it can replace.
    PresentModeSelectionInput input = MakeInput(VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR);
    input.presentModeSelectionSwapchainCount = 2;
    EXPECT_FALSE(DecidePresentModeSelection(input).forceCreatedMode);

    PresentModeSelectionInput presentInput = MakeInput(VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR);
    presentInput.presentSwapchainCount = 2;
    EXPECT_FALSE(DecidePresentModeSelection(presentInput).forceCreatedMode);
}

TEST(VulkanPresentChainPolicy, ADeeperSelectionIsReportedInsteadOfRewritten) {
    const PresentModeSelectionDecision decision = DecidePresentModeSelection(
        MakeInput(VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, /*chainHead=*/false));
    EXPECT_FALSE(decision.forceCreatedMode);
    EXPECT_TRUE(decision.blockedByChainPosition);
}

TEST(VulkanPresentChainPolicy, AChainWithoutASelectionDecidesNothing) {
    PresentModeSelectionInput input = MakeInput(VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR);
    input.hasPresentModeSelection = false;
    const PresentModeSelectionDecision decision = DecidePresentModeSelection(input);
    EXPECT_FALSE(decision.forceCreatedMode);
    EXPECT_FALSE(decision.blockedByChainPosition);
}

}  // namespace
