#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "../common/vulkan_layer_registration.h"

namespace {

using ce::vulkan_layer::BuildRegistrationPlan;
using ce::vulkan_layer::IsCaptureEngineLayerManifestPath;
using ce::vulkan_layer::PathToUtf8ForLogging;
using ce::vulkan_layer::RegistrationMode;
using ce::vulkan_layer::RegistryRoot;
using ce::vulkan_layer::RegistryView;
using ce::vulkan_layer::ShouldDeleteRegistryValueForTarget;

std::filesystem::path MakeValuePath(const wchar_t* fileName) {
    return std::filesystem::path(L"C:\\CaptureEngine") / std::filesystem::path(fileName);
}

void TouchFile(const std::filesystem::path& path) {
    std::ofstream out(PathToUtf8ForLogging(path), std::ios::binary);
    out << '\n';
}

}  // namespace

TEST(VulkanLayerRegistrationTest, CurrentUserPlanKeepsUsableManifestsInSharedHKCU) {
    const std::filesystem::path baseDir = std::filesystem::current_path() / "vk_reg_plan_hkcu";
    std::filesystem::create_directories(baseDir);

    const auto manifest64 = baseDir / L"VK_LAYER_CE_overlay.json";
    const auto library64 = baseDir / L"VK_LAYER_CE_overlay.dll";
    const auto manifest32 = baseDir / L"VK_LAYER_CE_overlay_x86.json";
    const auto library32 = baseDir / L"VK_LAYER_CE_overlay_x86.dll";

    TouchFile(manifest64);
    TouchFile(library64);
    TouchFile(manifest32);
    TouchFile(library32);

    const auto plan = BuildRegistrationPlan(baseDir, RegistrationMode::CurrentUser, false);
    ASSERT_EQ(plan.effectiveMode, RegistrationMode::CurrentUser);
    ASSERT_EQ(plan.installTargets.size(), 1u);
    EXPECT_EQ(plan.installTargets[0].root, RegistryRoot::CurrentUser);
    EXPECT_EQ(plan.installTargets[0].view, RegistryView::Default);
    ASSERT_EQ(plan.installTargets[0].manifests.size(), 2u);

    std::filesystem::remove_all(baseDir);
}

TEST(VulkanLayerRegistrationTest, ElevatedAutoPlanSplitsHKLMViewsByArchitecture) {
    const std::filesystem::path baseDir = std::filesystem::current_path() / "vk_reg_plan_hklm";
    std::filesystem::create_directories(baseDir);

    const auto manifest64 = baseDir / L"VK_LAYER_CE_overlay.json";
    const auto library64 = baseDir / L"VK_LAYER_CE_overlay.dll";
    const auto manifest32 = baseDir / L"VK_LAYER_CE_overlay_x86.json";
    const auto library32 = baseDir / L"VK_LAYER_CE_overlay_x86.dll";

    TouchFile(manifest64);
    TouchFile(library64);
    TouchFile(manifest32);
    TouchFile(library32);

    const auto plan = BuildRegistrationPlan(baseDir, RegistrationMode::Auto, true);
    ASSERT_EQ(plan.effectiveMode, RegistrationMode::AllUsers);
    ASSERT_EQ(plan.installTargets.size(), 2u);

    const auto& first = plan.installTargets[0];
    const auto& second = plan.installTargets[1];
    EXPECT_EQ(first.root, RegistryRoot::LocalMachine);
    EXPECT_EQ(second.root, RegistryRoot::LocalMachine);
    EXPECT_NE(first.view, second.view);

    const auto* x64Target = first.view == RegistryView::Registry64 ? &first : &second;
    const auto* x86Target = first.view == RegistryView::Registry32 ? &first : &second;
    ASSERT_EQ(x64Target->manifests.size(), 1u);
    ASSERT_EQ(x86Target->manifests.size(), 1u);
    EXPECT_FALSE(x64Target->manifests[0].is32Bit);
    EXPECT_TRUE(x86Target->manifests[0].is32Bit);

    std::filesystem::remove_all(baseDir);
}

TEST(VulkanLayerRegistrationTest, PlanSkipsMissingArchitectureArtifacts) {
    const std::filesystem::path baseDir = std::filesystem::current_path() / "vk_reg_plan_skip_missing";
    std::filesystem::create_directories(baseDir);

    const auto manifest64 = baseDir / L"VK_LAYER_CE_overlay.json";
    const auto library64 = baseDir / L"VK_LAYER_CE_overlay.dll";
    TouchFile(manifest64);
    TouchFile(library64);

    const auto plan = BuildRegistrationPlan(baseDir, RegistrationMode::Auto, true);
    ASSERT_EQ(plan.installTargets.size(), 1u);
    EXPECT_EQ(plan.installTargets[0].view, RegistryView::Registry64);
    ASSERT_EQ(plan.installTargets[0].manifests.size(), 1u);
    EXPECT_FALSE(plan.installTargets[0].manifests[0].is32Bit);

    std::filesystem::remove_all(baseDir);
}

TEST(VulkanLayerRegistrationTest, CaptureEngineManifestDetectionMatchesOnlyOwnedFiles) {
    EXPECT_TRUE(IsCaptureEngineLayerManifestPath(MakeValuePath(L"VK_LAYER_CE_overlay.json")));
    EXPECT_TRUE(IsCaptureEngineLayerManifestPath(MakeValuePath(L"VK_LAYER_CE_overlay_x86.json")));
    EXPECT_TRUE(IsCaptureEngineLayerManifestPath(MakeValuePath(L"VK_LAYER_CAPTURE_overlay.json")));
    EXPECT_FALSE(IsCaptureEngineLayerManifestPath(MakeValuePath(L"other_layer.json")));
}

TEST(VulkanLayerRegistrationTest, CleanupRemovesMismatchedTargetEntries) {
    const std::filesystem::path baseDir = std::filesystem::current_path() / "vk_reg_plan_cleanup";
    std::filesystem::create_directories(baseDir);

    const auto manifest64 = baseDir / L"VK_LAYER_CE_overlay.json";
    const auto library64 = baseDir / L"VK_LAYER_CE_overlay.dll";
    const auto manifest32 = baseDir / L"VK_LAYER_CE_overlay_x86.json";
    const auto library32 = baseDir / L"VK_LAYER_CE_overlay_x86.dll";

    TouchFile(manifest64);
    TouchFile(library64);
    TouchFile(manifest32);
    TouchFile(library32);

    const auto plan = BuildRegistrationPlan(baseDir, RegistrationMode::Auto, true);
    ASSERT_EQ(plan.installTargets.size(), 2u);

    EXPECT_TRUE(
        ShouldDeleteRegistryValueForTarget(plan, RegistryRoot::CurrentUser, RegistryView::Default, manifest64, true));
    EXPECT_TRUE(ShouldDeleteRegistryValueForTarget(plan, RegistryRoot::LocalMachine, RegistryView::Registry32,
                                                   manifest64, true));
    EXPECT_FALSE(ShouldDeleteRegistryValueForTarget(plan, RegistryRoot::LocalMachine, RegistryView::Registry64,
                                                    manifest64, true));
    EXPECT_TRUE(ShouldDeleteRegistryValueForTarget(plan, RegistryRoot::LocalMachine, RegistryView::Registry64,
                                                   MakeValuePath(L"VK_LAYER_CAPTURE_overlay.json"), false));
    EXPECT_FALSE(ShouldDeleteRegistryValueForTarget(plan, RegistryRoot::LocalMachine, RegistryView::Registry64,
                                                    MakeValuePath(L"third_party_layer.json"), false));

    std::filesystem::remove_all(baseDir);
}
