#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "../common/build_identity.h"
#include "../common/vulkan_layer_registration.h"

namespace {

using ce::vulkan_layer::BuildRegistrationPlan;
using ce::vulkan_layer::PathToUtf8ForLogging;
using ce::vulkan_layer::RegistrationMode;
using ce::vulkan_layer::RegistryRoot;
using ce::vulkan_layer::RegistryView;

void TouchFile(const std::filesystem::path& path) {
    std::ofstream out(PathToUtf8ForLogging(path), std::ios::binary);
    out << '\n';
}

}  // namespace

TEST(VulkanLayerRegistrationTest, CurrentUserPlanSplitsHKCUViewsByArchitecture) {
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
    ASSERT_EQ(plan.installTargets.size(), 2u);

    const auto& first = plan.installTargets[0];
    const auto& second = plan.installTargets[1];
    EXPECT_EQ(first.root, RegistryRoot::CurrentUser);
    EXPECT_EQ(second.root, RegistryRoot::CurrentUser);
    EXPECT_NE(first.view, second.view);

    const auto* x64Target = first.view == RegistryView::Registry64 ? &first : &second;
    const auto* x86Target = first.view == RegistryView::Registry32 ? &first : &second;
    ASSERT_EQ(x64Target->manifests.size(), 1u);
    ASSERT_EQ(x86Target->manifests.size(), 1u);
    EXPECT_EQ(x64Target->manifests[0].manifestPath, manifest64);
    EXPECT_EQ(x86Target->manifests[0].manifestPath, manifest32);
    EXPECT_EQ(x64Target->manifests[0].layerName,
              std::wstring(L"VK_LAYER_CE_overlay_b") + std::to_wstring(GetCurrentBuildNumber()));
    EXPECT_EQ(x86Target->manifests[0].layerName,
              std::wstring(L"VK_LAYER_CE_overlay_x86_b") + std::to_wstring(GetCurrentBuildNumber()));

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
    EXPECT_EQ(x64Target->manifests[0].manifestPath, manifest64);
    EXPECT_EQ(x86Target->manifests[0].manifestPath, manifest32);

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

TEST(VulkanLayerRegistrationSourceTest, RepairTargetsOwnedManifestNamesInWritableScopes) {
    const std::filesystem::path source = std::filesystem::current_path() / "common" / "vulkan_layer_registration.cpp";
    std::ifstream input(source, std::ios::binary);
    ASSERT_TRUE(input.is_open()) << source.string();
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());

    EXPECT_NE(text.find("RegEnumValueW"), std::string::npos);
    EXPECT_NE(text.find("VK_LAYER_CAPTURE_overlay.json"), std::string::npos);
    EXPECT_NE(text.find("IsOwnedManifestPath"), std::string::npos);

    const size_t locations = text.find("BuildRepairLocations(const RegistrationPlan& plan)");
    const size_t hkcu64 = text.find("RegistryRoot::CurrentUser, RegistryView::Registry64", locations);
    const size_t hkcu32 = text.find("RegistryRoot::CurrentUser, RegistryView::Registry32", locations);
    const size_t elevatedGate = text.find("if (plan.processElevated)", locations);
    const size_t hklm64 = text.find("RegistryRoot::LocalMachine, RegistryView::Registry64", locations);
    const size_t hklm32 = text.find("RegistryRoot::LocalMachine, RegistryView::Registry32", locations);
    ASSERT_NE(locations, std::string::npos);
    ASSERT_NE(hkcu64, std::string::npos);
    ASSERT_NE(hkcu32, std::string::npos);
    ASSERT_NE(elevatedGate, std::string::npos);
    ASSERT_NE(hklm64, std::string::npos);
    ASSERT_NE(hklm32, std::string::npos);
    EXPECT_LT(hkcu64, elevatedGate);
    EXPECT_LT(hkcu32, elevatedGate);
    EXPECT_LT(elevatedGate, hklm64);
    EXPECT_LT(elevatedGate, hklm32);

    const size_t deleteTarget = text.find("bool DeleteRegistryTarget(const RegistryTarget& target)");
    const size_t exactManifestLoop = text.find("for (const LayerManifest& manifest : target.manifests)", deleteTarget);
    const size_t exactValue = text.find("manifest.manifestPath.wstring()", exactManifestLoop);
    const size_t apply = text.find("bool ApplyRegistrationPlan(const RegistrationPlan& plan, bool install)");
    const size_t exactUnregister = text.find("DeleteRegistryTarget(target)", apply);
    ASSERT_NE(deleteTarget, std::string::npos);
    ASSERT_NE(exactManifestLoop, std::string::npos);
    ASSERT_NE(exactValue, std::string::npos);
    ASSERT_NE(apply, std::string::npos);
    ASSERT_NE(exactUnregister, std::string::npos);
    EXPECT_LT(deleteTarget, exactManifestLoop);
    EXPECT_LT(exactManifestLoop, exactValue);
    EXPECT_LT(apply, exactUnregister);
}

TEST(VulkanLayerRegistrationSourceTest, ControllerRetainsOneExactPlanThroughUnregistration) {
    const std::filesystem::path source = std::filesystem::current_path() / "captureengine" / "main.cpp";
    std::ifstream input(source, std::ios::binary);
    ASSERT_TRUE(input.is_open()) << source.string();
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());

    EXPECT_NE(text.find("ScopedVulkanRegistration() : plan_(BuildControllerVulkanRegistrationPlan())"),
              std::string::npos);
    const size_t repair = text.find("RepairOwnedRegistrations(plan_)");
    const size_t apply = text.find("ApplyRegistrationPlan(plan_, true)");
    ASSERT_NE(repair, std::string::npos);
    ASSERT_NE(apply, std::string::npos);
    EXPECT_LT(repair, apply);
    EXPECT_NE(text.find("ApplyRegistrationPlan(plan_, true)"), std::string::npos);
    EXPECT_NE(text.find("ApplyRegistrationPlan(plan_, false)"), std::string::npos);
    EXPECT_NE(text.find("std::call_once(unregistrationOnce_"), std::string::npos);
    EXPECT_NE(text.find("vulkanReg.Unregister();"), std::string::npos);
    EXPECT_NE(text.find("g_VulkanReg->Unregister()"), std::string::npos);
    EXPECT_EQ(text.find("Registry_ManageImplicitLayer"), std::string::npos);
}
