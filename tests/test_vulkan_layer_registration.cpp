#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "source_fragment_reader.h"

#include "../common/build_identity.h"
#include "../common/vulkan_layer_registration.h"
#include "../hook/vulkan_layer/vulkan_presentation_color.h"

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

TEST(VulkanPresentationColorTest, UsesSwapchainColorSpaceInsteadOfTenBitFormat) {
    using ce::presentation_color::Encoding;
    EXPECT_EQ(Encoding::Sdr709,
              ce::presentation_color::ResolveVulkan(VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                                    VK_COLOR_SPACE_SRGB_NONLINEAR_KHR));
    EXPECT_EQ(Encoding::Hdr10Pq,
              ce::presentation_color::ResolveVulkan(VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                                    VK_COLOR_SPACE_HDR10_ST2084_EXT));
    EXPECT_EQ(Encoding::LinearScRgb,
              ce::presentation_color::ResolveVulkan(VK_FORMAT_R16G16B16A16_SFLOAT,
                                                    VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT));
    EXPECT_EQ(Encoding::Unsupported,
              ce::presentation_color::ResolveVulkan(VK_FORMAT_R16G16B16A16_SFLOAT,
                                                    VK_COLOR_SPACE_SRGB_NONLINEAR_KHR));
}

TEST(VulkanLayerRegistrationTest, CurrentUserPlanSplitsHKCUViewsByArchitecture) {
    const std::filesystem::path baseDir = std::filesystem::current_path() / "vk_reg_plan_hkcu";
    const std::filesystem::path stagingDir = baseDir / "staging";
    std::filesystem::create_directories(baseDir);

    const auto manifest64 = baseDir / L"VK_LAYER_CE_overlay.json";
    const auto library64 = baseDir / L"VK_LAYER_CE_overlay.dll";
    const auto manifest32 = baseDir / L"VK_LAYER_CE_overlay_x86.json";
    const auto library32 = baseDir / L"VK_LAYER_CE_overlay_x86.dll";

    TouchFile(manifest64);
    TouchFile(library64);
    TouchFile(baseDir / L"VK_LAYER_CE_gate.dll");
    TouchFile(manifest32);
    TouchFile(library32);
    TouchFile(baseDir / L"VK_LAYER_CE_gate_x86.dll");

    const auto plan = BuildRegistrationPlan(baseDir, RegistrationMode::CurrentUser, false, stagingDir);
    ASSERT_EQ(plan.effectiveMode, RegistrationMode::CurrentUser);
    ASSERT_EQ(plan.stagingDir, stagingDir);
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
    EXPECT_EQ(x64Target->manifests[0].sourceManifestPath, manifest64);
    EXPECT_EQ(x86Target->manifests[0].sourceManifestPath, manifest32);
    EXPECT_EQ(x64Target->manifests[0].manifestPath, stagingDir / L"VK_LAYER_CE_overlay.json");
    EXPECT_EQ(x86Target->manifests[0].manifestPath, stagingDir / L"VK_LAYER_CE_overlay_x86.json");
    EXPECT_EQ(x64Target->manifests[0].layerName,
              std::wstring(L"VK_LAYER_CE_overlay_b") + std::to_wstring(GetCurrentBuildNumber()));
    EXPECT_EQ(x86Target->manifests[0].layerName,
              std::wstring(L"VK_LAYER_CE_overlay_x86_b") + std::to_wstring(GetCurrentBuildNumber()));

    std::filesystem::remove_all(baseDir);
}

TEST(VulkanLayerRegistrationTest, ElevatedAutoPlanSplitsHKLMViewsByArchitecture) {
    const std::filesystem::path baseDir = std::filesystem::current_path() / "vk_reg_plan_hklm";
    const std::filesystem::path stagingDir = baseDir / "staging";
    std::filesystem::create_directories(baseDir);

    const auto manifest64 = baseDir / L"VK_LAYER_CE_overlay.json";
    const auto library64 = baseDir / L"VK_LAYER_CE_overlay.dll";
    const auto manifest32 = baseDir / L"VK_LAYER_CE_overlay_x86.json";
    const auto library32 = baseDir / L"VK_LAYER_CE_overlay_x86.dll";

    TouchFile(manifest64);
    TouchFile(library64);
    TouchFile(baseDir / L"VK_LAYER_CE_gate.dll");
    TouchFile(manifest32);
    TouchFile(library32);
    TouchFile(baseDir / L"VK_LAYER_CE_gate_x86.dll");

    const auto plan = BuildRegistrationPlan(baseDir, RegistrationMode::Auto, true, stagingDir);
    ASSERT_EQ(plan.effectiveMode, RegistrationMode::AllUsers);
    ASSERT_EQ(plan.stagingDir, stagingDir);
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
    EXPECT_EQ(x64Target->manifests[0].sourceManifestPath, manifest64);
    EXPECT_EQ(x86Target->manifests[0].sourceManifestPath, manifest32);
    EXPECT_EQ(x64Target->manifests[0].manifestPath, stagingDir / L"VK_LAYER_CE_overlay.json");
    EXPECT_EQ(x86Target->manifests[0].manifestPath, stagingDir / L"VK_LAYER_CE_overlay_x86.json");

    std::filesystem::remove_all(baseDir);
}

TEST(VulkanLayerRegistrationTest, PlanSkipsMissingArchitectureArtifacts) {
    const std::filesystem::path baseDir = std::filesystem::current_path() / "vk_reg_plan_skip_missing";
    const std::filesystem::path stagingDir = baseDir / "staging";
    std::filesystem::create_directories(baseDir);

    const auto manifest64 = baseDir / L"VK_LAYER_CE_overlay.json";
    const auto library64 = baseDir / L"VK_LAYER_CE_overlay.dll";
    TouchFile(manifest64);
    TouchFile(library64);
    TouchFile(baseDir / L"VK_LAYER_CE_gate.dll");

    const auto plan = BuildRegistrationPlan(baseDir, RegistrationMode::Auto, true, stagingDir);
    ASSERT_EQ(plan.installTargets.size(), 1u);
    EXPECT_EQ(plan.installTargets[0].view, RegistryView::Registry64);
    ASSERT_EQ(plan.installTargets[0].manifests.size(), 1u);
    EXPECT_FALSE(plan.installTargets[0].manifests[0].is32Bit);

    std::filesystem::remove_all(baseDir);
}

TEST(VulkanLayerRegistrationTest, DefaultStagingDirectoryResolvesExpectedSubdirectory) {
    std::filesystem::path userStaging;
    ASSERT_TRUE(ce::vulkan_layer::ResolveDefaultStagingDirectory(RegistrationMode::CurrentUser, &userStaging));
    EXPECT_FALSE(userStaging.empty());
    const std::wstring userStr = userStaging.wstring();
    EXPECT_NE(userStr.find(L"CaptureEngine\\vulkan_layers\\b"), std::wstring::npos);

    std::filesystem::path allUsersStaging;
    ASSERT_TRUE(ce::vulkan_layer::ResolveDefaultStagingDirectory(RegistrationMode::AllUsers, &allUsersStaging));
    EXPECT_FALSE(allUsersStaging.empty());
    const std::wstring allStr = allUsersStaging.wstring();
    EXPECT_NE(allStr.find(L"CaptureEngine\\vulkan_layers\\b"), std::wstring::npos);
}

TEST(VulkanLayerRegistrationTest, StagingCopiesArtifactsAndCleanupRemovesOldBuilds) {
    const std::filesystem::path testRoot = std::filesystem::current_path() / "vk_reg_staging_test";
    const std::filesystem::path baseDir = testRoot / "installed";
    const std::filesystem::path stagingParent = testRoot / "vulkan_layers";
    const std::filesystem::path activeStaging = stagingParent / ("b" + std::to_string(GetCurrentBuildNumber()));
    const std::filesystem::path oldStaging = stagingParent / "b99998";

    std::filesystem::create_directories(baseDir);
    std::filesystem::create_directories(oldStaging);

    const auto srcManifest = baseDir / L"VK_LAYER_CE_overlay.json";
    const auto srcLib = baseDir / L"VK_LAYER_CE_overlay.dll";
    TouchFile(srcManifest);
    TouchFile(srcLib);
    TouchFile(baseDir / L"VK_LAYER_CE_gate.dll");
    TouchFile(oldStaging / L"VK_LAYER_CE_overlay.dll");

    const auto plan = BuildRegistrationPlan(baseDir, RegistrationMode::CurrentUser, false, activeStaging);
    EXPECT_EQ(plan.stagingDir, activeStaging);
    ASSERT_EQ(plan.installTargets.size(), 1u);

    // Call CleanupStaleStagingDirectories and verify oldStaging is pruned
    EXPECT_TRUE(std::filesystem::exists(oldStaging));
    EXPECT_TRUE(ce::vulkan_layer::CleanupStaleStagingDirectories(plan));
    EXPECT_FALSE(std::filesystem::exists(oldStaging));

    std::filesystem::remove_all(testRoot);
}

TEST(VulkanLayerRegistrationTest, ApplyRegistrationPlanNeutralizesLegacyManifestsInBaseDir) {
    const std::filesystem::path testRoot = std::filesystem::current_path() / "vk_reg_neutralize_test";
    const std::filesystem::path baseDir = testRoot / "installed";
    const std::filesystem::path stagingDir = testRoot / "staging";
    std::filesystem::create_directories(baseDir);
    std::filesystem::create_directories(stagingDir);

    const auto legacyManifest64 = baseDir / L"VK_LAYER_CE_overlay.json";
    const auto legacyManifest32 = baseDir / L"VK_LAYER_CE_overlay_x86.json";
    const auto lib64 = baseDir / L"VK_LAYER_CE_overlay.dll";
    TouchFile(legacyManifest64);
    TouchFile(legacyManifest32);
    TouchFile(lib64);
    TouchFile(baseDir / L"VK_LAYER_CE_gate.dll");

    const auto plan = BuildRegistrationPlan(baseDir, RegistrationMode::CurrentUser, false, stagingDir);
    EXPECT_TRUE(std::filesystem::exists(legacyManifest64));
    EXPECT_TRUE(std::filesystem::exists(legacyManifest32));

    EXPECT_TRUE(ce::vulkan_layer::ApplyRegistrationPlan(plan, true));

    // Staged manifest was created in stagingDir:
    EXPECT_TRUE(std::filesystem::exists(stagingDir / L"VK_LAYER_CE_overlay.json"));
    // Legacy manifests in baseDir were purged:
    EXPECT_FALSE(std::filesystem::exists(legacyManifest64));
    EXPECT_FALSE(std::filesystem::exists(legacyManifest32));

    // Cleanup registry and test directory
    ce::vulkan_layer::ApplyRegistrationPlan(plan, false);
    std::filesystem::remove_all(testRoot);
}

// The manifest names the negotiation gate, which loads the full layer from its
// own directory. A full layer without its gate is not registrable: registering
// the full layer directly would map ~1.5 MB and its imports into every Vulkan
// process on the machine again.
TEST(VulkanLayerRegistrationTest, PlanRequiresTheGateBesideTheFullLayer) {
    const std::filesystem::path baseDir = std::filesystem::current_path() / "vk_reg_plan_gate";
    const std::filesystem::path stagingDir = baseDir / "staging";
    std::filesystem::create_directories(baseDir);
    TouchFile(baseDir / L"VK_LAYER_CE_overlay.json");
    TouchFile(baseDir / L"VK_LAYER_CE_overlay.dll");

    const auto withoutGate = BuildRegistrationPlan(baseDir, RegistrationMode::CurrentUser, false, stagingDir);
    EXPECT_TRUE(withoutGate.installTargets.empty());
    ASSERT_FALSE(withoutGate.manifests.empty());
    EXPECT_TRUE(withoutGate.manifests[0].libraryExists);
    EXPECT_FALSE(withoutGate.manifests[0].gateExists);

    TouchFile(baseDir / L"VK_LAYER_CE_gate.dll");
    const auto withGate = BuildRegistrationPlan(baseDir, RegistrationMode::CurrentUser, false, stagingDir);
    ASSERT_EQ(withGate.installTargets.size(), 1u);
    const auto& manifest = withGate.installTargets[0].manifests.at(0);
    EXPECT_EQ(manifest.gatePath, stagingDir / L"VK_LAYER_CE_gate.dll");
    EXPECT_EQ(manifest.libraryPath, stagingDir / L"VK_LAYER_CE_overlay.dll");
    EXPECT_EQ(manifest.gatePath.parent_path(), manifest.libraryPath.parent_path())
        << "the gate loads the full layer from its own directory";

    std::filesystem::remove_all(baseDir);
}

TEST(VulkanLayerRegistrationSourceTest, ManifestNamesTheGateAndBothImagesAreStaged) {
    const std::string text = ce::test_source::ReadFile(std::filesystem::current_path() / "common" /
                                                       "vulkan_layer_registration.cpp");
    ASSERT_FALSE(text.empty());
    const size_t manifest = text.find("static bool WriteStagedManifest(const LayerManifest& manifest) {");
    const size_t manifestEnd = text.find("static bool StagePlanArtifacts(", manifest);
    ASSERT_NE(manifest, std::string::npos);
    ASSERT_NE(manifestEnd, std::string::npos);
    const std::string json = text.substr(manifest, manifestEnd - manifest);
    EXPECT_NE(json.find("manifest.gatePath.filename()"), std::string::npos);
    EXPECT_EQ(json.find("manifest.libraryPath"), std::string::npos) << "the full layer must never be the manifest's";
    EXPECT_EQ(json.find("vkGetInstanceProcAddr"), std::string::npos)
        << "the gate exports negotiation only; the manifest must not name proc-address exports";

    const size_t stage = text.find("static bool StagePlanArtifacts(");
    const size_t stageEnd = text.find("void LogRegistrationPlan(", stage);
    ASSERT_NE(stageEnd, std::string::npos);
    const std::string staging = text.substr(stage, stageEnd - stage);
    const size_t full = staging.find("StageFileIfChanged(manifest.sourceLibraryPath, manifest.libraryPath)");
    const size_t gate = staging.find("StageFileIfChanged(manifest.sourceGatePath, manifest.gatePath)");
    ASSERT_NE(full, std::string::npos);
    ASSERT_NE(gate, std::string::npos);
    EXPECT_LT(full, gate) << "stage the full layer before the gate that loads it";
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

// The Vulkan loader composes a process's layer chain once, inside
// vkCreateInstance. A controller that unregisters its implicit layer on exit
// therefore makes Vulkan late injection structurally impossible: a title started
// while CaptureEngine is not running never carries the layer, and no later
// injection can add one. Session logs/20260818_224257 recorded exactly that
// failure for Strange Brigade Vulkan. Registration must outlive the controller.
TEST(VulkanLayerRegistrationSourceTest, ControllerKeepsLayerRegistrationResidentAcrossShutdown) {
    const std::filesystem::path residencySource =
        std::filesystem::current_path() / "captureengine" / "main_vulkan_residency.h";
    const std::string residency = ce::test_source::ReadFile(residencySource);
    ASSERT_FALSE(residency.empty()) << residencySource.string();

    EXPECT_NE(residency.find("VulkanLayerResidency() : plan_("), std::string::npos);
    EXPECT_NE(residency.find("BuildControllerVulkanRegistrationPlan())"), std::string::npos);
    const size_t repair = residency.find("RepairOwnedRegistrations(plan_)");
    const size_t apply = residency.find("ApplyRegistrationPlan(plan_, true)");
    ASSERT_NE(repair, std::string::npos);
    ASSERT_NE(apply, std::string::npos);
    EXPECT_LT(repair, apply);

    // The owner must have no teardown path at all: no destructor, no unregister
    // call, nothing that can put ApplyRegistrationPlan into uninstall mode.
    EXPECT_EQ(residency.find("ApplyRegistrationPlan(plan_, false)"), std::string::npos);
    EXPECT_EQ(residency.find("~VulkanLayerResidency"), std::string::npos);

    const std::filesystem::path source = std::filesystem::current_path() / "captureengine" / "main.cpp";
    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty()) << source.string();

    // Neither the console handler nor the normal shutdown sequence may drop it.
    EXPECT_NE(text.find("VulkanLayerResidency vulkanReg"), std::string::npos);
    EXPECT_EQ(text.find("ApplyRegistrationPlan(plan_, false)"), std::string::npos);
    EXPECT_EQ(text.find("vulkanReg.Unregister();"), std::string::npos);
    EXPECT_EQ(text.find("g_VulkanReg->Unregister()"), std::string::npos);
    EXPECT_EQ(text.find("Registry_ManageImplicitLayer"), std::string::npos);
}

TEST(VulkanLayerRegistrationTest, StaleEntrySelectionPrunesSupersededOwnedManifestsOnly) {
    const std::vector<std::wstring> existing = {
        L"C:\\Old\\Install\\VK_LAYER_CE_overlay.json",
        L"C:\\Current\\VK_LAYER_CE_overlay.json",
        L"C:\\Legacy\\VK_LAYER_CAPTURE_overlay.json",
        L"C:\\Old\\Install\\VK_LAYER_CE_overlay_x86.json",
    };
    const std::vector<std::wstring> retained = {L"C:\\Current\\VK_LAYER_CE_overlay.json"};

    const auto stale = ce::vulkan_layer::SelectStaleOwnedEntries(existing, retained);
    ASSERT_EQ(stale.size(), 3u);
    EXPECT_EQ(stale[0], L"C:\\Old\\Install\\VK_LAYER_CE_overlay.json");
    EXPECT_EQ(stale[1], L"C:\\Legacy\\VK_LAYER_CAPTURE_overlay.json");
    EXPECT_EQ(stale[2], L"C:\\Old\\Install\\VK_LAYER_CE_overlay_x86.json");
}

// Resident registration means CE prunes this key on every start rather than on
// exit, so a bug here would silently disable Steam's, OBS's, RTSS's, or EOS's
// Vulkan overlay on the user's machine. Foreign manifests are never eligible.
TEST(VulkanLayerRegistrationTest, StaleEntrySelectionNeverTouchesForeignImplicitLayers) {
    const std::vector<std::wstring> existing = {
        L"C:\\Program Files (x86)\\Steam\\SteamOverlayVulkanLayer64.json",
        L"C:\\Program Files (x86)\\Steam\\SteamFossilizeVulkanLayer64.json",
        L"C:\\ProgramData\\obs-studio-hook\\obs-vulkan64.json",
        L"C:\\Program Files (x86)\\RivaTuner Statistics Server\\Vulkan\\RTSSVkLayer64.json",
        L"C:\\Program Files (x86)\\Epic Games\\Epic Online Services\\EOSOverlayVkLayer-Win64.json",
    };

    EXPECT_TRUE(ce::vulkan_layer::SelectStaleOwnedEntries(existing, {}).empty());
}

TEST(VulkanLayerRegistrationTest, StaleEntrySelectionRetainsLiveEntryCaseInsensitively) {
    const std::vector<std::wstring> existing = {L"C:\\Current\\vk_layer_ce_overlay.json"};
    const std::vector<std::wstring> retained = {L"C:\\CURRENT\\VK_LAYER_CE_overlay.json"};

    // Retaining the live entry instead of deleting and rewriting it is what keeps
    // the registration continuously readable by a concurrent vkCreateInstance.
    EXPECT_TRUE(ce::vulkan_layer::SelectStaleOwnedEntries(existing, retained).empty());
}
