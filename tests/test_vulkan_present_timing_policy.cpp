#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../hook/vulkan_layer/vulkan_present_timing_policy.h"
#include "source_fragment_reader.h"

namespace {

using ce::vulkan_present_timing_policy::DecideDevice;
using ce::vulkan_present_timing_policy::DeviceInput;
using ce::vulkan_present_timing_policy::IsFifoPresentMode;
using ce::vulkan_present_timing_policy::PresentInput;
using ce::vulkan_present_timing_policy::ScanChainForStructureType;
using ce::vulkan_present_timing_policy::ShouldEnableSwapchain;
using ce::vulkan_present_timing_policy::ShouldInjectRelativeTiming;
using ce::vulkan_present_timing_policy::ShouldRefreshTimingProperties;
using ce::vulkan_present_timing_policy::ShouldRefreshTimeDomains;
using ce::vulkan_present_timing_policy::SwapchainInput;

DeviceInput FullySupportedDevice() {
    DeviceInput input = {};
    input.vblankPacingRequested = true;
    input.surfaceCapabilities2Enabled = true;
    input.swapchainExtensionEnabled = true;
    input.presentTimingExtensionAvailable = true;
    input.presentId2ExtensionAvailable = true;
    input.calibratedTimestampsExtensionAvailable = true;
    input.featureQueryAvailable = true;
    input.presentTimingFeatureSupported = true;
    input.relativeTimeFeatureSupported = true;
    input.applicationFeatureChainInspectable = true;
    return input;
}

TEST(VulkanPresentTimingPolicy, LayerEnablesSupportedRelativeTiming) {
    const auto decision = DecideDevice(FullySupportedDevice());
    EXPECT_TRUE(decision.enable);
    EXPECT_TRUE(decision.addFeatureNode);
}

TEST(VulkanPresentTimingPolicy, ApplicationFeatureChoiceIsNeverOverwritten) {
    DeviceInput input = FullySupportedDevice();
    input.applicationSpecifiedFeature = true;
    input.applicationEnabledPresentTiming = true;
    input.applicationEnabledRelativeTime = false;
    EXPECT_FALSE(DecideDevice(input).enable);

    input.applicationEnabledRelativeTime = true;
    const auto enabled = DecideDevice(input);
    EXPECT_TRUE(enabled.enable);
    EXPECT_FALSE(enabled.addFeatureNode);
}

TEST(VulkanPresentTimingPolicy, EveryExtensionAndFeatureDependencyIsRequired) {
    DeviceInput input = FullySupportedDevice();
    input.presentTimingExtensionAvailable = false;
    EXPECT_FALSE(DecideDevice(input).enable);
    input = FullySupportedDevice();
    input.presentId2ExtensionAvailable = false;
    EXPECT_FALSE(DecideDevice(input).enable);
    input = FullySupportedDevice();
    input.calibratedTimestampsExtensionAvailable = false;
    EXPECT_FALSE(DecideDevice(input).enable);
    input = FullySupportedDevice();
    input.surfaceCapabilities2Enabled = false;
    EXPECT_FALSE(DecideDevice(input).enable);
    input = FullySupportedDevice();
    input.relativeTimeFeatureSupported = false;
    EXPECT_FALSE(DecideDevice(input).enable);
    input = FullySupportedDevice();
    input.applicationFeatureChainInspectable = false;
    EXPECT_FALSE(DecideDevice(input).enable);
}

TEST(VulkanPresentTimingPolicy, SurfaceMustSupportRelativeTimingOnAFifoMode) {
    SwapchainInput input = {};
    input.vblankPacingRequested = true;
    input.deviceEnabled = true;
    input.surfaceQueryAvailable = true;
    input.surfaceQuerySucceeded = true;
    input.presentTimingSupportedForSurface = true;
    input.relativeTimeSupportedForSurface = true;
    input.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    EXPECT_TRUE(ShouldEnableSwapchain(input));
    input.presentMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    EXPECT_TRUE(ShouldEnableSwapchain(input));
    input.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    EXPECT_FALSE(ShouldEnableSwapchain(input));
    input.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    EXPECT_FALSE(ShouldEnableSwapchain(input));
    input.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    input.relativeTimeSupportedForSurface = false;
    EXPECT_FALSE(ShouldEnableSwapchain(input));
    input.relativeTimeSupportedForSurface = true;
    input.presentTimingSupportedForSurface = false;
    EXPECT_FALSE(ShouldEnableSwapchain(input));
    input.presentTimingSupportedForSurface = true;
    input.vblankPacingRequested = false;
    EXPECT_FALSE(ShouldEnableSwapchain(input));
}

TEST(VulkanPresentTimingPolicy, RelativeTimingIsOnlyInjectedForOneUnclaimedSwapchain) {
    PresentInput input = {};
    input.vblankPacingRequested = true;
    input.swapchainEnabled = true;
    input.fifoPresentModeActive = true;
    input.swapchainCount = 1;
    input.refreshDurationNs = 6'944'444;
    input.applicationChainInspectable = true;
    EXPECT_TRUE(ShouldInjectRelativeTiming(input));
    input.swapchainCount = 2;
    EXPECT_FALSE(ShouldInjectRelativeTiming(input));
    input.swapchainCount = 1;
    input.refreshDurationNs = 0;
    EXPECT_FALSE(ShouldInjectRelativeTiming(input));
    input.refreshDurationNs = 6'944'444;
    input.applicationAlreadySpecifiedTiming = true;
    EXPECT_FALSE(ShouldInjectRelativeTiming(input));
    input.applicationAlreadySpecifiedTiming = false;
    input.applicationChainInspectable = false;
    EXPECT_FALSE(ShouldInjectRelativeTiming(input));
    input.applicationChainInspectable = true;
    input.vblankPacingRequested = false;
    EXPECT_FALSE(ShouldInjectRelativeTiming(input));
    input.vblankPacingRequested = true;
    input.fifoPresentModeActive = false;
    EXPECT_FALSE(ShouldInjectRelativeTiming(input));
}

TEST(VulkanPresentTimingPolicy, TimingPropertiesAreRetriedThenRefreshedPeriodically) {
    EXPECT_TRUE(ShouldRefreshTimingProperties(1, 0));
    EXPECT_TRUE(ShouldRefreshTimingProperties(17, 0));
    EXPECT_FALSE(ShouldRefreshTimingProperties(1, 6'944'444));
    EXPECT_FALSE(ShouldRefreshTimingProperties(2, 6'944'444));
    EXPECT_FALSE(ShouldRefreshTimingProperties(255, 6'944'444));
    EXPECT_TRUE(ShouldRefreshTimingProperties(256, 6'944'444));
    EXPECT_TRUE(ShouldRefreshTimeDomains(0, false));
    EXPECT_FALSE(ShouldRefreshTimeDomains(1, true));
    EXPECT_TRUE(ShouldRefreshTimeDomains(256, true));
}

TEST(VulkanPresentTimingPolicy, ChainScanFindsTimingAndTerminatesOnCycles) {
    VkBaseInStructure tail = {};
    tail.sType = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT;
    VkBaseInStructure head = {};
    head.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
    head.pNext = &tail;
    EXPECT_TRUE(ScanChainForStructureType(&head, VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT).found);
    const auto absent = ScanChainForStructureType(&head, VK_STRUCTURE_TYPE_PRESENT_INFO_KHR);
    EXPECT_FALSE(absent.found);
    EXPECT_TRUE(absent.complete);

    head.pNext = &head;
    const auto cyclic = ScanChainForStructureType(&head, VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT);
    EXPECT_FALSE(cyclic.found);
    EXPECT_FALSE(cyclic.complete);
}

TEST(VulkanPresentTimingPolicy, FifoClassificationIsExact) {
    EXPECT_TRUE(IsFifoPresentMode(VK_PRESENT_MODE_FIFO_KHR));
    EXPECT_TRUE(IsFifoPresentMode(VK_PRESENT_MODE_FIFO_RELAXED_KHR));
    EXPECT_FALSE(IsFifoPresentMode(VK_PRESENT_MODE_MAILBOX_KHR));
    EXPECT_FALSE(IsFifoPresentMode(VK_PRESENT_MODE_IMMEDIATE_KHR));
}

TEST(VulkanPresentTimingSourcePolicy, CeilingUsesNativeRelativeSchedulingWithoutAWaitLoop) {
    namespace fs = std::filesystem;
    const std::string timing = ce::test_source::ReadFile(
        fs::current_path() / "hook" / "vulkan_layer" / "vulkan_present_timing.cpp");
    const std::string present = ce::test_source::ReadFile(
        fs::current_path() / "hook" / "vulkan_layer" / "vulkan_layer_present.cpp");
    const std::string capabilities = ce::test_source::ReadFile(
        fs::current_path() / "hook" / "vulkan_layer" / "vulkan_layer_capabilities.cpp");
    const std::string hooks = ce::test_source::ReadFile(
        fs::current_path() / "hook" / "vulkan_layer" / "vulkan_layer_hooks.cpp");
    const std::string remix = ce::test_source::ReadFile(
        fs::current_path() / "hook" / "apis" / "remix_hook.cpp");
    ASSERT_FALSE(timing.empty());
    ASSERT_FALSE(present.empty());
    ASSERT_FALSE(capabilities.empty());
    ASSERT_FALSE(hooks.empty());
    ASSERT_FALSE(remix.empty());

    EXPECT_NE(timing.find("VK_PRESENT_TIMING_INFO_PRESENT_AT_RELATIVE_TIME_BIT_EXT"), std::string::npos);
    EXPECT_NE(timing.find("timingInfo.targetTime = refreshDuration"), std::string::npos);
    EXPECT_NE(timing.find("timingInfo.presentStageQueries = 0"), std::string::npos);
    EXPECT_NE(timing.find("timingsInfo.pNext = nextChain"), std::string::npos)
        << "the CE timing head must retain every application-owned present node";
    EXPECT_NE(timing.find("VK_KHR_SURFACE_EXTENSION_NAME"), std::string::npos)
        << "surface-capabilities2 must not be injected without its base instance extension";
    EXPECT_NE(hooks.find("vkCreateInstance rejected CE's optional native present-timing query"),
              std::string::npos)
        << "an optional timing-query extension must never prevent application startup";
    EXPECT_EQ(present.find("suppressPresentMetering"), std::string::npos)
        << "native timing must compose with, not remove, generated-frame metering";
    EXPECT_NE(present.find("preserved for generated-frame spacing"), std::string::npos);
    EXPECT_EQ(capabilities.find("CopyWithoutPresentMetering"), std::string::npos)
        << "device extension enumeration must preserve NVIDIA's metering capability";
    EXPECT_EQ(remix.find("enablePresentMetering"), std::string::npos)
        << "CE must not force Remix away from hardware generated-frame spacing";
    EXPECT_EQ(timing.find("VK_PRESENT_TIMING_INFO_PRESENT_AT_NEAREST_REFRESH_CYCLE_BIT_EXT"),
              std::string::npos);
    const size_t firstDeviceFallback = hooks.find("CreateDeviceWithPresentTimingFallback");
    ASSERT_NE(firstDeviceFallback, std::string::npos);
    EXPECT_NE(hooks.find("CreateDeviceWithPresentTimingFallback", firstDeviceFallback + 1),
              std::string::npos)
        << "dropping CE's optional overlay queue must still retry without optional timing additions";
    for (const char* forbidden : {"Sleep(", "SleepEx(", "WaitForSingleObject(", "std::this_thread::sleep"})
        EXPECT_EQ(timing.find(forbidden), std::string::npos) << forbidden;
}

}  // namespace
