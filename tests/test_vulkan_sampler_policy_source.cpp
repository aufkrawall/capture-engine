#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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
    const std::string source = ReadVulkanLayerSource();
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("!borderAddress"), std::string::npos);
    EXPECT_NE(source.find("modified.unnormalizedCoordinates == VK_FALSE"), std::string::npos);
    EXPECT_NE(source.find("standardMinMag"), std::string::npos);
    EXPECT_NE(source.find("modified.compareEnable == VK_FALSE"), std::string::npos);
    EXPECT_NE(source.find("!specialReduction"), std::string::npos);
}

TEST(VulkanSamplerPolicySourceTest, RetriesOriginalDescriptorTransactionally) {
    const std::string source = ReadVulkanLayerSource();
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("if (result != VK_SUCCESS && changed)"), std::string::npos);
    EXPECT_NE(source.find("fp_vkCreateSampler(device, pCreateInfo, pAllocator, pSampler)"), std::string::npos);
}
