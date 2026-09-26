#include <gtest/gtest.h>
#include <windows.h>

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

    // Which keys are pruned, and what each retains, is covered behaviorally by
    // the RepairScopes tests below; the repair must go through that policy.
    const size_t repair = text.find("bool RepairOwnedRegistrations(const RegistrationPlan& plan)");
    ASSERT_NE(repair, std::string::npos);
    EXPECT_NE(text.find("BuildRepairScopes(plan)", repair), std::string::npos);

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

namespace {

void TouchBothArchitectureLayerSources(const std::filesystem::path& baseDir) {
    std::filesystem::create_directories(baseDir);
    TouchFile(baseDir / L"VK_LAYER_CE_overlay.json");
    TouchFile(baseDir / L"VK_LAYER_CE_overlay.dll");
    TouchFile(baseDir / L"VK_LAYER_CE_gate.dll");
    TouchFile(baseDir / L"VK_LAYER_CE_overlay_x86.json");
    TouchFile(baseDir / L"VK_LAYER_CE_overlay_x86.dll");
    TouchFile(baseDir / L"VK_LAYER_CE_gate_x86.dll");
}

}  // namespace

// HKCU\Software is shared by both registry views, so the x64 and x86 entries sit
// in ONE key. Pruning each view against only its own architecture deleted both
// live entries on every start (logs/20260926_044427: "Removed superseded CE
// manifest entry from HKCU/64-bit: ...overlay_x86.json" and the mirror line for
// HKCU/32-bit), leaving any Vulkan title that started in the gap without the layer.
TEST(VulkanLayerRegistrationTest, RepairScopesPruneSharedHKCUKeyOnceRetainingBothArchitectures) {
    const std::filesystem::path baseDir = std::filesystem::current_path() / "vk_reg_repair_hkcu";
    const std::filesystem::path stagingDir = baseDir / "staging";
    TouchBothArchitectureLayerSources(baseDir);

    const auto plan = BuildRegistrationPlan(baseDir, RegistrationMode::CurrentUser, false, stagingDir);
    ASSERT_EQ(plan.installTargets.size(), 2u);

    const auto scopes = ce::vulkan_layer::BuildRepairScopes(plan);
    ASSERT_EQ(scopes.size(), 1u);
    EXPECT_EQ(scopes[0].root, RegistryRoot::CurrentUser);
    EXPECT_EQ(scopes[0].view, RegistryView::Default);

    const std::wstring live64 = (stagingDir / L"VK_LAYER_CE_overlay.json").wstring();
    const std::wstring live32 = (stagingDir / L"VK_LAYER_CE_overlay_x86.json").wstring();
    const std::wstring superseded = L"C:\\Old\\b1\\VK_LAYER_CE_overlay_x86.json";
    const std::vector<std::wstring> existing = {live64, live32, superseded};

    const auto stale = ce::vulkan_layer::SelectStaleOwnedEntries(existing, scopes[0].retainedValueNames);
    ASSERT_EQ(stale.size(), 1u);
    EXPECT_EQ(stale[0], superseded);

    std::filesystem::remove_all(baseDir);
}

// HKLM\Software IS redirected, so its views are separate keys: an x86 manifest in
// the 64-bit view is a genuine wrong-view leftover and must still be pruned. The
// all-users plan retains nothing in HKCU, so a previous per-user registration is
// removed rather than shadowing the machine-wide one.
TEST(VulkanLayerRegistrationTest, RepairScopesKeepRedirectedHKLMViewsPerArchitecture) {
    const std::filesystem::path baseDir = std::filesystem::current_path() / "vk_reg_repair_hklm";
    const std::filesystem::path stagingDir = baseDir / "staging";
    TouchBothArchitectureLayerSources(baseDir);

    const auto plan = BuildRegistrationPlan(baseDir, RegistrationMode::Auto, true, stagingDir);
    ASSERT_EQ(plan.effectiveMode, RegistrationMode::AllUsers);

    const auto scopes = ce::vulkan_layer::BuildRepairScopes(plan);
    ASSERT_EQ(scopes.size(), 3u);
    const ce::vulkan_layer::RepairScope* hkcu = nullptr;
    const ce::vulkan_layer::RepairScope* hklm64 = nullptr;
    const ce::vulkan_layer::RepairScope* hklm32 = nullptr;
    for (const auto& scope : scopes) {
        if (scope.root == RegistryRoot::CurrentUser) hkcu = &scope;
        if (scope.root == RegistryRoot::LocalMachine && scope.view == RegistryView::Registry64) hklm64 = &scope;
        if (scope.root == RegistryRoot::LocalMachine && scope.view == RegistryView::Registry32) hklm32 = &scope;
    }
    ASSERT_NE(hkcu, nullptr);
    ASSERT_NE(hklm64, nullptr);
    ASSERT_NE(hklm32, nullptr);

    const std::wstring live64 = (stagingDir / L"VK_LAYER_CE_overlay.json").wstring();
    const std::wstring live32 = (stagingDir / L"VK_LAYER_CE_overlay_x86.json").wstring();
    EXPECT_TRUE(hkcu->retainedValueNames.empty());
    ASSERT_EQ(hklm64->retainedValueNames.size(), 1u);
    ASSERT_EQ(hklm32->retainedValueNames.size(), 1u);
    EXPECT_EQ(hklm64->retainedValueNames[0], live64);
    EXPECT_EQ(hklm32->retainedValueNames[0], live32);

    const auto stale64 = ce::vulkan_layer::SelectStaleOwnedEntries({live64, live32}, hklm64->retainedValueNames);
    ASSERT_EQ(stale64.size(), 1u);
    EXPECT_EQ(stale64[0], live32);

    std::filesystem::remove_all(baseDir);
}

TEST(VulkanLayerRegistrationTest, RepairScopesSkipHKLMWithoutElevation) {
    const std::filesystem::path baseDir = std::filesystem::current_path() / "vk_reg_repair_unelevated";
    TouchBothArchitectureLayerSources(baseDir);

    const auto plan = BuildRegistrationPlan(baseDir, RegistrationMode::Auto, false, baseDir / "staging");
    const auto scopes = ce::vulkan_layer::BuildRepairScopes(plan);
    ASSERT_EQ(scopes.size(), 1u);
    EXPECT_EQ(scopes[0].root, RegistryRoot::CurrentUser);
    EXPECT_EQ(scopes[0].retainedValueNames.size(), 2u);

    std::filesystem::remove_all(baseDir);
}

// The premise behind the single HKCU scope, checked against the running OS: a
// value written through the 64-bit view of HKCU\Software is visible through the
// 32-bit view. If Windows ever redirected HKCU\Software, this fails and the
// shared-view assumption in BuildRepairScopes must be revisited.
TEST(VulkanLayerRegistrationTest, HKCUSoftwareIsSharedBetweenRegistryViews) {
    constexpr wchar_t kProbeKey[] = L"SOFTWARE\\CaptureEngineTests\\VulkanRegViewShare";
    constexpr wchar_t kProbeValue[] = L"probe";
    RegDeleteTreeW(HKEY_CURRENT_USER, kProbeKey);

    HKEY key64 = nullptr;
    ASSERT_EQ(RegCreateKeyExW(HKEY_CURRENT_USER, kProbeKey, 0, nullptr, REG_OPTION_VOLATILE,
                              KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key64, nullptr),
              ERROR_SUCCESS);
    const DWORD written = 0x5A;
    EXPECT_EQ(RegSetValueExW(key64, kProbeValue, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&written),
                             sizeof(written)),
              ERROR_SUCCESS);
    RegCloseKey(key64);

    HKEY key32 = nullptr;
    DWORD read = 0;
    DWORD size = sizeof(read);
    const LONG openResult = RegOpenKeyExW(HKEY_CURRENT_USER, kProbeKey, 0, KEY_QUERY_VALUE | KEY_WOW64_32KEY, &key32);
    if (openResult == ERROR_SUCCESS) {
        EXPECT_EQ(RegQueryValueExW(key32, kProbeValue, nullptr, nullptr, reinterpret_cast<BYTE*>(&read), &size),
                  ERROR_SUCCESS);
        RegCloseKey(key32);
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, kProbeKey);
    RegDeleteKeyW(HKEY_CURRENT_USER, L"SOFTWARE\\CaptureEngineTests");

    ASSERT_EQ(openResult, ERROR_SUCCESS);
    EXPECT_EQ(read, written);
}
