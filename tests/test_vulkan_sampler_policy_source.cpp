#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "../hook/vulkan_layer/vulkan_sampler_policy.h"

namespace {

std::string ReadVulkanLayerSource() {
    const auto path = std::filesystem::current_path() / "hook" / "vulkan_layer" / "vulkan_layer.cpp";
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream contents;
    contents << stream.rdbuf();
    return contents.str();
}

}  // namespace

TEST(VulkanSamplerPolicySourceTest, ProtectsStructurallyUnsafeSamplersInAllModes) {
    ce::vulkan_sampler_policy::Input input = {};
    input.mipmapped = true;
    input.standardMinMag = true;
    EXPECT_TRUE(ce::vulkan_sampler_policy::Classify(input).allowMipMapping);

    input.borderAddress = true;
    EXPECT_EQ(ce::vulkan_sampler_policy::Classify(input).mipMappingReason,
              ce::vulkan_sampler_policy::RejectReason::BorderAddress);
    input.borderAddress = false;
    input.unnormalizedCoordinates = true;
    EXPECT_EQ(ce::vulkan_sampler_policy::Classify(input).mipMappingReason,
              ce::vulkan_sampler_policy::RejectReason::UnnormalizedCoordinates);
    input.unnormalizedCoordinates = false;
    input.specialReduction = true;
    EXPECT_EQ(ce::vulkan_sampler_policy::Classify(input).mipMappingReason,
              ce::vulkan_sampler_policy::RejectReason::SpecialReduction);
}

TEST(VulkanSamplerPolicySourceTest, RetriesOriginalDescriptorTransactionally) {
    const std::string source = ReadVulkanLayerSource();
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("if (result != VK_SUCCESS && changed)"), std::string::npos);
    EXPECT_NE(source.find("fp_vkCreateSampler(device, pCreateInfo, pAllocator, pSampler)"), std::string::npos);
}
