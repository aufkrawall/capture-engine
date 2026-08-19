#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "../hook/vulkan_layer/vulkan_swapchain_image_policy.h"
#include "source_fragment_reader.h"

namespace {

using ce::vulkan_swapchain_image_policy::Decide;
using ce::vulkan_swapchain_image_policy::Decision;
using ce::vulkan_swapchain_image_policy::Input;

Input MakeInput(int32_t configured, uint32_t applicationMin, uint32_t surfaceMin, uint32_t surfaceMax) {
    Input input = {};
    input.configuredBackbufferCount = configured;
    input.applicationMinImageCount = applicationMin;
    input.surfaceCapabilitiesKnown = true;
    input.surfaceMinImageCount = surfaceMin;
    input.surfaceMaxImageCount = surfaceMax;
    return input;
}

std::string StripComments(const std::string& source) {
    std::string stripped;
    stripped.reserve(source.size());
    for (size_t index = 0; index < source.size();) {
        if (source.compare(index, 2, "//") == 0) {
            const size_t lineEnd = source.find('\n', index);
            if (lineEnd == std::string::npos)
                break;
            index = lineEnd;
            continue;
        }
        if (source.compare(index, 2, "/*") == 0) {
            const size_t blockEnd = source.find("*/", index + 2);
            if (blockEnd == std::string::npos)
                break;
            index = blockEnd + 2;
            continue;
        }
        stripped.push_back(source[index]);
        ++index;
    }
    return stripped;
}

std::string ReadVulkanLayerPresentSource() {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / "hook" / "vulkan_layer" /
                                              "vulkan_layer_present.cpp");
}

}  // namespace

// The DOOM Eternal regression: with "present from compute" off the game asks for
// three images and keeps two acquired. Forcing the configured two back onto it
// removed the acquire headroom and NVIDIA's WSI answered the blocking acquire
// with VK_NOT_READY, which the game turns into a fatal error.
TEST(VulkanSwapchainImagePolicyTest, KeepsTheGamesCountWhenTheConfiguredCountIsLower) {
    const Decision decision = Decide(MakeInput(2, 3, 2, 0));

    EXPECT_EQ(decision.minImageCount, 3u);
    EXPECT_FALSE(decision.overrideApplied);
    EXPECT_TRUE(decision.reductionSkipped);
    EXPECT_FALSE(decision.clampedToSurfaceMaximum);
}

TEST(VulkanSwapchainImagePolicyTest, AppliesTheConfiguredCountWhenItRaisesTheGamesRequest) {
    const Decision decision = Decide(MakeInput(3, 2, 2, 0));

    EXPECT_EQ(decision.minImageCount, 3u);
    EXPECT_TRUE(decision.overrideApplied);
    EXPECT_FALSE(decision.reductionSkipped);
}

TEST(VulkanSwapchainImagePolicyTest, LeavesAMatchingRequestAlone) {
    const Decision decision = Decide(MakeInput(2, 2, 2, 0));

    EXPECT_EQ(decision.minImageCount, 2u);
    EXPECT_FALSE(decision.overrideApplied);
    EXPECT_FALSE(decision.reductionSkipped);
}

TEST(VulkanSwapchainImagePolicyTest, DisabledAndInvalidConfiguredCountsNeverTouchTheRequest) {
    for (const int32_t configured : {-1, 0, 1}) {
        const Decision decision = Decide(MakeInput(configured, 3, 2, 0));
        EXPECT_EQ(decision.minImageCount, 3u) << "configured=" << configured;
        EXPECT_FALSE(decision.overrideApplied) << "configured=" << configured;
        EXPECT_FALSE(decision.reductionSkipped) << "configured=" << configured;
        EXPECT_FALSE(decision.skippedUnknownCapabilities) << "configured=" << configured;
    }
}

TEST(VulkanSwapchainImagePolicyTest, ClampsAConfiguredCountAboveTheSurfaceMaximum) {
    const Decision decision = Decide(MakeInput(6, 2, 2, 3));

    EXPECT_EQ(decision.minImageCount, 3u);
    EXPECT_TRUE(decision.overrideApplied);
    EXPECT_TRUE(decision.clampedToSurfaceMaximum);
}

// Clamping to the surface maximum must not become a back door for the reduction
// this policy exists to prevent.
TEST(VulkanSwapchainImagePolicyTest, SurfaceMaximumClampNeverFallsBelowTheGamesRequest) {
    const Decision decision = Decide(MakeInput(6, 4, 2, 3));

    EXPECT_EQ(decision.minImageCount, 4u);
    EXPECT_FALSE(decision.overrideApplied);
    EXPECT_FALSE(decision.clampedToSurfaceMaximum);
}

TEST(VulkanSwapchainImagePolicyTest, RaisesToTheSurfaceMinimum) {
    const Decision decision = Decide(MakeInput(2, 2, 3, 0));

    EXPECT_EQ(decision.minImageCount, 3u);
    EXPECT_TRUE(decision.overrideApplied);
}

TEST(VulkanSwapchainImagePolicyTest, DeclinesTheOverrideWithoutSurfaceCapabilities) {
    Input input = {};
    input.configuredBackbufferCount = 3;
    input.applicationMinImageCount = 2;
    input.surfaceCapabilitiesKnown = false;

    const Decision decision = Decide(input);

    EXPECT_EQ(decision.minImageCount, 2u);
    EXPECT_FALSE(decision.overrideApplied);
    EXPECT_TRUE(decision.skippedUnknownCapabilities);
}

TEST(VulkanSwapchainImagePolicySourceTest, SwapchainCreationRoutesThroughThePolicy) {
    const std::string source = ReadVulkanLayerPresentSource();
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("ce::vulkan_swapchain_image_policy::Decide(imagePolicyInput)"), std::string::npos);
    EXPECT_NE(source.find("fp_vkGetPhysicalDeviceSurfaceCapabilitiesKHR"), std::string::npos);
    // The unconditional assignment that produced the DOOM Eternal fatal error.
    EXPECT_EQ(source.find("modifiedCI.minImageCount = (uint32_t)bbCount"), std::string::npos);
}

// DOOM Eternal session `20260819_034454`: the game's stderr was an inherited
// pipe nobody drained, so one diagnostic write from the layer parked the present
// thread in NtWriteFile forever. An injected layer owns none of the host's
// standard streams and must never write to them.
TEST(VulkanSwapchainImagePolicySourceTest, LayerNeverWritesToTheHostStandardStreams) {
    const std::filesystem::path layerDir = std::filesystem::current_path() / "hook" / "vulkan_layer";
    ASSERT_TRUE(std::filesystem::is_directory(layerDir)) << layerDir.string();

    std::vector<std::string> offenders;
    size_t inspected = 0;
    for (const auto& entry : std::filesystem::directory_iterator(layerDir)) {
        const std::string extension = entry.path().extension().string();
        if (extension != ".cpp" && extension != ".h")
            continue;
        const std::string code = StripComments(ce::test_source::ReadFile(entry.path()));
        ++inspected;
        if (code.find("stderr") != std::string::npos || code.find("stdout") != std::string::npos)
            offenders.push_back(entry.path().filename().string());
    }

    EXPECT_GT(inspected, 0u);
    EXPECT_TRUE(offenders.empty()) << "Vulkan layer sources writing to the host's standard streams: " << [&] {
        std::string joined;
        for (const std::string& name : offenders)
            joined += name + " ";
        return joined;
    }();
}
