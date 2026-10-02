#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "hook/vulkan_layer/vulkan_swapchain_result_policy.h"
#include "source_fragment_reader.h"

namespace {

using ce::vulkan_swapchain_result_policy::InvalidationResultName;
namespace result = ce::vulkan_swapchain_result_policy;

std::string ReadLayerSource(const char* name) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / "hook" / "vulkan_layer" / name);
}

TEST(VulkanSwapchainResultPolicyTest, NamesEveryResultThatInvalidatesTheSwapchain) {
    EXPECT_STREQ(InvalidationResultName(result::kSuboptimal), "VK_SUBOPTIMAL_KHR");
    EXPECT_STREQ(InvalidationResultName(result::kOutOfDate), "VK_ERROR_OUT_OF_DATE_KHR");
    EXPECT_STREQ(InvalidationResultName(result::kSurfaceLost), "VK_ERROR_SURFACE_LOST_KHR");
    EXPECT_STREQ(InvalidationResultName(result::kFullScreenExclusiveModeLost),
                 "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT");
    EXPECT_STREQ(InvalidationResultName(result::kDeviceLost), "VK_ERROR_DEVICE_LOST");
}

TEST(VulkanSwapchainResultPolicyTest, OrdinaryResultsStayUnnamed) {
    EXPECT_EQ(InvalidationResultName(0), nullptr);            // VK_SUCCESS
    EXPECT_EQ(InvalidationResultName(1), nullptr);            // VK_NOT_READY
    EXPECT_EQ(InvalidationResultName(2), nullptr);            // VK_TIMEOUT
    EXPECT_EQ(InvalidationResultName(-1), nullptr);           // VK_ERROR_OUT_OF_HOST_MEMORY
    EXPECT_EQ(InvalidationResultName(-3), nullptr);           // VK_ERROR_INITIALIZATION_FAILED
}

// DOOM Eternal session 20260928_042343 recreated its swapchain eighteen times
// without the log saying why. Both acquire entry points and the present path
// must report the result they hand back, and must hand it back unchanged.
TEST(VulkanSwapchainResultPolicyTest, PresentAndBothAcquiresReportInvalidationResults) {
    const std::string swapchain = ReadLayerSource("vulkan_layer_swapchain.cpp");
    ASSERT_FALSE(swapchain.empty());
    EXPECT_NE(swapchain.find("LogSwapchainInvalidationResult(\"vkAcquireNextImageKHR\", acquireResult"),
              std::string::npos);
    // Both acquire variants end through the boundary that logs.
    size_t boundaryCalls = 0;
    for (size_t at = swapchain.find("EndAcquireBoundary(sd, acquireResult, pImageIndex);"); at != std::string::npos;
         at = swapchain.find("EndAcquireBoundary(sd, acquireResult, pImageIndex);", at + 1)) {
        ++boundaryCalls;
    }
    EXPECT_EQ(boundaryCalls, 2u);
    EXPECT_NE(swapchain.find("static_assert(swapchain_result::kFullScreenExclusiveModeLost == "
                             "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT);"),
              std::string::npos);

    const std::string present = ReadLayerSource("vulkan_layer_present.cpp");
    ASSERT_FALSE(present.empty());
    const size_t downCall = present.find("res = disp->fp_vkQueuePresentKHR(");
    const size_t logged = present.find("LogSwapchainInvalidationResult(\"vkQueuePresentKHR\", res,");
    ASSERT_NE(downCall, std::string::npos);
    ASSERT_NE(logged, std::string::npos);
    EXPECT_LT(downCall, logged);
    // The present hook ends where the next hook begins.
    const size_t hookEnd = present.find("VKAPI_ATTR", downCall);
    ASSERT_NE(hookEnd, std::string::npos);
    EXPECT_GT(present.find("res = ", downCall + 1), hookEnd)
        << "the present hook must return the driver's result unchanged";
}

}  // namespace
