#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <string>

#include "../hook/vulkan_layer/vulkan_sharpen_route_policy.h"
#include "source_fragment_reader.h"

namespace {

using ce::vulkan_sharpen_route::Choose;
using ce::vulkan_sharpen_route::Identity;
using ce::vulkan_sharpen_route::Input;
using ce::vulkan_sharpen_route::MustRebuild;
using ce::vulkan_sharpen_route::RefusalReason;
using ce::vulkan_sharpen_route::Route;

Input GraphicsQueue() {
    Input input;
    input.queueFamilyKnown = true;
    input.queueSupportsGraphics = true;
    input.queueSupportsCompute = true;
    return input;
}

// DOOM Eternal's "present from compute" family: compute, no graphics.
Input ComputeOnlyQueue() {
    Input input;
    input.queueFamilyKnown = true;
    input.queueSupportsCompute = true;
    input.swapchainHasStorageUsage = true;
    input.storageWriteWithoutFormat = true;
    return input;
}

Identity BuiltIdentity() {
    Identity identity;
    identity.swapchain = 0x28298A200D0ull;
    identity.format = 44;
    identity.width = 3840;
    identity.height = 2160;
    identity.imageCount = 2;
    identity.queueFamily = 0;
    identity.route = Route::kGraphics;
    return identity;
}

std::string ReadLayerSource(const char* name) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / "hook" / "vulkan_layer" / name);
}

}  // namespace

TEST(VulkanSharpenRoutePolicyTest, GraphicsQueueUsesTheRenderPass) {
    EXPECT_EQ(Choose(GraphicsQueue()), Route::kGraphics);
}

// Session `20260923_003755`: DOOM moved its present to the compute-only family 2
// and the render-pass route kept submitting there until the device was lost.
TEST(VulkanSharpenRoutePolicyTest, ComputeOnlyQueueNeverGetsTheRenderPass) {
    EXPECT_EQ(Choose(ComputeOnlyQueue()), Route::kCompute);
}

TEST(VulkanSharpenRoutePolicyTest, ComputeRouteNeedsStorageUsage) {
    Input input = ComputeOnlyQueue();
    input.swapchainHasStorageUsage = false;
    EXPECT_EQ(Choose(input), Route::kNone);
    EXPECT_STREQ(RefusalReason(input), "compute_present_without_storage_usage");
}

// The kernel writes the application's format through a formatless storage
// image, which is only legal where the device and the format both allow it.
TEST(VulkanSharpenRoutePolicyTest, ComputeRouteNeedsFormatlessStorageWrites) {
    Input input = ComputeOnlyQueue();
    input.storageWriteWithoutFormat = false;
    EXPECT_EQ(Choose(input), Route::kNone);
    EXPECT_STREQ(RefusalReason(input), "compute_present_format_not_storage_writable");
}

TEST(VulkanSharpenRoutePolicyTest, UnknownFamilyRunsNothing) {
    Input input = GraphicsQueue();
    input.queueFamilyKnown = false;
    EXPECT_EQ(Choose(input), Route::kNone);
    EXPECT_STREQ(RefusalReason(input), "present_queue_family_unknown");
}

TEST(VulkanSharpenRoutePolicyTest, TransferOnlyQueueRunsNothing) {
    Input input;
    input.queueFamilyKnown = true;
    input.swapchainHasStorageUsage = true;
    input.storageWriteWithoutFormat = true;
    EXPECT_EQ(Choose(input), Route::kNone);
    EXPECT_STREQ(RefusalReason(input), "present_queue_has_no_graphics_or_compute");
}

TEST(VulkanSharpenRoutePolicyTest, IdenticalIdentityKeepsTheState) {
    EXPECT_FALSE(MustRebuild(BuiltIdentity(), BuiltIdentity()));
}

// The family moved without a swapchain recreate, so every swapchain-derived
// field still matches. The command pool belongs to the old family.
TEST(VulkanSharpenRoutePolicyTest, PresentFamilyChangeRebuilds) {
    Identity current = BuiltIdentity();
    current.queueFamily = 2;
    EXPECT_TRUE(MustRebuild(BuiltIdentity(), current));
}

TEST(VulkanSharpenRoutePolicyTest, RouteChangeRebuilds) {
    Identity current = BuiltIdentity();
    current.route = Route::kCompute;
    EXPECT_TRUE(MustRebuild(BuiltIdentity(), current));
}

TEST(VulkanSharpenRoutePolicyTest, SwapchainGenerationChangeRebuilds) {
    const Identity built = BuiltIdentity();
    Identity current = built;
    current.swapchain = 0x28298A215D0ull;
    EXPECT_TRUE(MustRebuild(built, current));
    current = built;
    current.format = 64;
    EXPECT_TRUE(MustRebuild(built, current));
    current = built;
    current.width = 2560;
    EXPECT_TRUE(MustRebuild(built, current));
    current = built;
    current.height = 1440;
    EXPECT_TRUE(MustRebuild(built, current));
    current = built;
    current.imageCount = 3;
    EXPECT_TRUE(MustRebuild(built, current));
}

// The per-present path has to feed both the route and the family into the
// rebuild decision; comparing only the swapchain is what let a family-0 command
// buffer reach a family-2 queue.
TEST(VulkanSharpenRouteSourceTest, PresentPathRebuildsThroughThePolicy) {
    const std::string source = ReadLayerSource("layer_sharpen.cpp");
    ASSERT_FALSE(source.empty());
    const size_t entry = source.find("bool SharpenPresentedFrame(");
    ASSERT_NE(entry, std::string::npos);
    const size_t choose = source.find("ce::vulkan_sharpen_route::Choose(routeInput)", entry);
    const size_t rebuild = source.find("ce::vulkan_sharpen_route::MustRebuild(SharpenStateIdentity(state), current)",
                                       entry);
    const size_t init = source.find("InitializeSharpenState(", entry);
    ASSERT_NE(choose, std::string::npos);
    ASSERT_NE(rebuild, std::string::npos);
    ASSERT_NE(init, std::string::npos);
    EXPECT_LT(choose, rebuild);
    EXPECT_LT(rebuild, init);
    EXPECT_NE(source.find("current.queueFamily = queueFamily;", entry), std::string::npos);
    EXPECT_NE(source.find("current.route = route;", entry), std::string::npos);
}

TEST(VulkanSharpenRouteSourceTest, ComputeRouteRecordsADispatch) {
    const std::string source = ReadLayerSource("layer_sharpen_compute.cpp");
    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("fp_vkCmdDispatch("), std::string::npos);
    EXPECT_NE(source.find("VK_PIPELINE_BIND_POINT_COMPUTE"), std::string::npos);
    EXPECT_EQ(source.find("fp_vkCmdBeginRenderPass"), std::string::npos)
        << "a compute-only present queue cannot execute a render pass";
}
