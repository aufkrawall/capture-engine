#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
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
using ce::vulkan_present_metering_policy::ClearPresentMeteringFeature;
using ce::vulkan_present_metering_policy::kStructureTypePhysicalDevicePresentMeteringFeaturesNV;
using ce::vulkan_present_metering_policy::CopyWithoutPresentMetering;
using ce::vulkan_present_metering_policy::ExtensionFilterResult;
using ce::vulkan_present_metering_policy::IsPresentMeteringExtensionName;
using ce::vulkan_present_metering_policy::PhysicalDevicePresentMeteringFeaturesNV;
using ce::vulkan_present_metering_policy::ShouldWithholdPresentMeteringCapability;

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

// --- Withholding the capability instead of suppressing the request --------
//
// Session `20260830_175147` (Portal RTX, 4x MFG, `vsync_mode=fifo`) proved the
// request-level suppression above cannot restore the vertical-blank wait: the
// layer suppressed metering from the moment the frame-generation swapchain was
// created and CE's DXGI interception still recorded "final Present1 #1024
// force=1 SyncInterval=0->1 Flags=0x200->0x0" seven seconds later. What was
// left was an unpaced burst quantized onto consecutive vertical blanks, so a
// group of four generated frames was drawn across 4/refresh no matter whether
// the rendered frame it belonged to took 21 ms or 48 ms - flat present-side
// frame times, visibly stuttering motion.

VkExtensionProperties MakeExtension(const char* name) {
    VkExtensionProperties properties = {};
    std::snprintf(properties.extensionName, sizeof(properties.extensionName), "%s", name);
    properties.specVersion = 1;
    return properties;
}

PhysicalDevicePresentMeteringFeaturesNV MakeMeteringFeature(VkBool32 enabled, void* next = nullptr) {
    PhysicalDevicePresentMeteringFeaturesNV features = {};
    features.sType = kStructureTypePhysicalDevicePresentMeteringFeaturesNV;
    features.pNext = next;
    features.presentMetering = enabled;
    return features;
}

TEST(VulkanPresentMeteringCapability, WithheldForExactlyTheModesThatCarryARateContract) {
    EXPECT_TRUE(ShouldWithholdPresentMeteringCapability("fifo"));
    EXPECT_TRUE(ShouldWithholdPresentMeteringCapability("adaptive"));
    EXPECT_FALSE(ShouldWithholdPresentMeteringCapability("mailbox"));
    EXPECT_FALSE(ShouldWithholdPresentMeteringCapability("off"));
    EXPECT_FALSE(ShouldWithholdPresentMeteringCapability("default"));
    EXPECT_FALSE(ShouldWithholdPresentMeteringCapability(""));
}

TEST(VulkanPresentMeteringCapability, ExtensionNameMatchIsExact) {
    EXPECT_TRUE(IsPresentMeteringExtensionName("VK_NV_present_metering"));
    EXPECT_FALSE(IsPresentMeteringExtensionName(nullptr));
    EXPECT_FALSE(IsPresentMeteringExtensionName(""));
    // A prefix or suffix must never take another extension away from the game.
    EXPECT_FALSE(IsPresentMeteringExtensionName("VK_NV_present_metering2"));
    EXPECT_FALSE(IsPresentMeteringExtensionName("VK_NV_present_meterin"));
    EXPECT_FALSE(IsPresentMeteringExtensionName("VK_NV_present_barrier"));
}

TEST(VulkanPresentMeteringCapability, CountOnlyPassReportsTheFilteredTotal) {
    const VkExtensionProperties source[] = {MakeExtension("VK_KHR_swapchain"),
                                            MakeExtension("VK_NV_present_metering"),
                                            MakeExtension("VK_KHR_present_id")};

    const ExtensionFilterResult result = CopyWithoutPresentMetering(source, 3, nullptr, 0);
    EXPECT_EQ(result.filteredTotal, 2u);
    EXPECT_EQ(result.removedCount, 1u);
    EXPECT_EQ(result.writtenCount, 0u);
    EXPECT_FALSE(result.truncated);
}

TEST(VulkanPresentMeteringCapability, ABufferSizedFromTheFilteredCountIsNotTruncated) {
    // This is the two-call idiom the filter exists to keep intact: an
    // application that sized its buffer from the count above must get
    // VK_SUCCESS, not VK_INCOMPLETE, on the second call.
    const VkExtensionProperties source[] = {MakeExtension("VK_KHR_swapchain"),
                                            MakeExtension("VK_NV_present_metering"),
                                            MakeExtension("VK_KHR_present_id")};
    VkExtensionProperties destination[2] = {};

    const ExtensionFilterResult result = CopyWithoutPresentMetering(source, 3, destination, 2);
    EXPECT_EQ(result.writtenCount, 2u);
    EXPECT_EQ(result.filteredTotal, 2u);
    EXPECT_EQ(result.removedCount, 1u);
    EXPECT_FALSE(result.truncated);
    EXPECT_STREQ(destination[0].extensionName, "VK_KHR_swapchain");
    EXPECT_STREQ(destination[1].extensionName, "VK_KHR_present_id");
}

TEST(VulkanPresentMeteringCapability, AnUndersizedBufferStillReportsTruncation) {
    const VkExtensionProperties source[] = {MakeExtension("VK_KHR_swapchain"),
                                            MakeExtension("VK_NV_present_metering"),
                                            MakeExtension("VK_KHR_present_id")};
    VkExtensionProperties destination[1] = {};

    const ExtensionFilterResult result = CopyWithoutPresentMetering(source, 3, destination, 1);
    EXPECT_EQ(result.writtenCount, 1u);
    EXPECT_EQ(result.filteredTotal, 2u);
    EXPECT_TRUE(result.truncated);
    EXPECT_STREQ(destination[0].extensionName, "VK_KHR_swapchain");
}

TEST(VulkanPresentMeteringCapability, ADriverListWithoutMeteringIsCopiedUnchanged) {
    const VkExtensionProperties source[] = {MakeExtension("VK_KHR_swapchain"), MakeExtension("VK_NV_present_barrier")};
    VkExtensionProperties destination[2] = {};

    const ExtensionFilterResult result = CopyWithoutPresentMetering(source, 2, destination, 2);
    EXPECT_EQ(result.removedCount, 0u);
    EXPECT_EQ(result.writtenCount, 2u);
    EXPECT_FALSE(result.truncated);
    EXPECT_STREQ(destination[0].extensionName, "VK_KHR_swapchain");
    EXPECT_STREQ(destination[1].extensionName, "VK_NV_present_barrier");
}

TEST(VulkanPresentMeteringCapability, AnEmptyDriverListIsHandled) {
    const ExtensionFilterResult nullSource = CopyWithoutPresentMetering(nullptr, 7, nullptr, 0);
    EXPECT_EQ(nullSource.filteredTotal, 0u);
    EXPECT_EQ(nullSource.removedCount, 0u);
    EXPECT_FALSE(nullSource.truncated);
}

TEST(VulkanPresentMeteringCapability, ClearsTheFeatureFlagWhereverItSitsInTheChain) {
    PhysicalDevicePresentMeteringFeaturesNV metering = MakeMeteringFeature(VK_TRUE);
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, nullptr, VK_FALSE};
    timeline.timelineSemaphore = VK_TRUE;
    timeline.pNext = &metering;

    EXPECT_EQ(ClearPresentMeteringFeature(&timeline), 1u);
    EXPECT_EQ(metering.presentMetering, VK_FALSE);
    // Nothing else in the application's output chain may be touched.
    EXPECT_EQ(timeline.timelineSemaphore, VK_TRUE);
}

TEST(VulkanPresentMeteringCapability, ClearsAHeadFeatureNode) {
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, nullptr, VK_FALSE};
    PhysicalDevicePresentMeteringFeaturesNV metering = MakeMeteringFeature(VK_TRUE, &timeline);

    EXPECT_EQ(ClearPresentMeteringFeature(&metering), 1u);
    EXPECT_EQ(metering.presentMetering, VK_FALSE);
}

TEST(VulkanPresentMeteringCapability, AnAlreadyUnsupportedFeatureIsNotCountedAsCleared) {
    PhysicalDevicePresentMeteringFeaturesNV metering = MakeMeteringFeature(VK_FALSE);
    EXPECT_EQ(ClearPresentMeteringFeature(&metering), 0u);
    EXPECT_EQ(ClearPresentMeteringFeature(nullptr), 0u);
}

TEST(VulkanPresentMeteringCapability, ASelfReferentialFeatureChainTerminates) {
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, nullptr, VK_FALSE};
    timeline.pNext = &timeline;
    EXPECT_EQ(ClearPresentMeteringFeature(&timeline), 0u);
}

}  // namespace
