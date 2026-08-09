#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "source_fragment_reader.h"

namespace {

std::string ReadProjectSource(const std::filesystem::path& relativePath) {
    const std::string source =
        ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
    EXPECT_FALSE(source.empty()) << relativePath.string();
    return source;
}

}  // namespace

TEST(RayReconstructionForceSourceTest, UsesPersistentValidatedCVarStorageInsteadOfEngineVtableCalls) {
    const std::string source = ReadProjectSource("hook/main_ue5.cpp");

    EXPECT_NE(source.find("kDenoiserModeCVar"), std::string::npos);
    EXPECT_NE(source.find("InterlockedCompareExchangePointer"), std::string::npos);
    EXPECT_NE(source.find("IsUniquelyStrongCandidate"), std::string::npos);
    EXPECT_NE(source.find("RestoreOverride"), std::string::npos);
    EXPECT_EQ(source.find("FindConsoleVariable"), std::string::npos);
    EXPECT_EQ(source.find("r.NGX.DLSS.RayReconstruction"), std::string::npos);
    EXPECT_EQ(source.find("Sleep("), std::string::npos);
}

TEST(RayReconstructionForceSourceTest, ObservesRealNgxCapabilityAndEvaluationLifecycleWithoutSpoofing) {
    const std::string source = ReadProjectSource("hook/apis/nvngx_hook.cpp");

    EXPECT_NE(source.find("SuperSamplingDenoising.Available"), std::string::npos);
    EXPECT_NE(source.find("SuperSamplingDenoising.FeatureInitResult"), std::string::npos);
    EXPECT_NE(source.find("NVSDK_NGX_D3D12_EvaluateFeature_C"), std::string::npos);
    EXPECT_NE(source.find("NVSDK_NGX_D3D12_ReleaseFeature"), std::string::npos);
    EXPECT_NE(source.find("NVSDK_NGX_VULKAN_CreateFeature1"), std::string::npos);
    EXPECT_NE(source.find("Feature 13 evaluation succeeded"), std::string::npos);
    EXPECT_EQ(source.find("RayReconstruction.Available"), std::string::npos);
    EXPECT_EQ(source.find("Spoofing Ray Reconstruction"), std::string::npos);
    EXPECT_EQ(source.find("OutSupported->FeatureSupported ="), std::string::npos);
}

TEST(RayReconstructionForceSourceTest, PublishesResolvedPolicyAndRestoresBeforeHookUnload) {
    const std::string host = ReadProjectSource("captureengine/inject_main.cpp");
    const std::string hookCommon = ReadProjectSource("hook/common/hook_common.cpp");
    const std::string hookThread = ReadProjectSource("hook/main_hookthread.cpp");

    EXPECT_NE(host.find("graphicsConfig.forceRayReconstruction = config.graphics.forceRayReconstruction"),
              std::string::npos);
    EXPECT_NE(hookCommon.find("mergedConfig.forceRayReconstruction = shmGfx.forceRayReconstruction"),
              std::string::npos);
    EXPECT_NE(hookThread.find("RefreshRayReconstructionOverride(activeGraphicsConfig.forceRayReconstruction)"),
              std::string::npos);
    EXPECT_NE(hookThread.find("ShutdownRayReconstructionOverride()"), std::string::npos);
}

TEST(RayReconstructionForceSourceTest, InstallsGraphicsHooksBeforePotentiallyExpensiveEngineDiscovery) {
    const std::string hookThread = ReadProjectSource("hook/main_hookthread.cpp");

    const size_t initialHookInstall = hookThread.find("CheckAndInstallHooks();");
    const size_t initialRRRefresh =
        hookThread.find("RefreshRayReconstructionOverride(initialGraphicsConfig.forceRayReconstruction)");
    ASSERT_NE(initialHookInstall, std::string::npos);
    ASSERT_NE(initialRRRefresh, std::string::npos);
    EXPECT_LT(initialHookInstall, initialRRRefresh);

    const size_t periodicHookInstall = hookThread.find("CheckAndInstallHooks();", initialHookInstall + 1);
    const size_t periodicRRRefresh =
        hookThread.find("RefreshRayReconstructionOverride(activeGraphicsConfig.forceRayReconstruction)");
    ASSERT_NE(periodicHookInstall, std::string::npos);
    ASSERT_NE(periodicRRRefresh, std::string::npos);
    EXPECT_LT(periodicHookInstall, periodicRRRefresh);
}

TEST(RayReconstructionForceSourceTest, CountsCandidateReferencesInOneCombinedExecutableScan) {
    const std::string source = ReadProjectSource("hook/main_ue5.cpp");

    const size_t layoutValidation = source.find("ValidateCandidateLayout(image, candidate);");
    const size_t invalidLayoutFilter = source.find("candidates.erase(std::remove_if");
    const size_t combinedReferenceScan = source.find("CountCandidateReferences(image, candidates);");
    ASSERT_NE(layoutValidation, std::string::npos);
    ASSERT_NE(invalidLayoutFilter, std::string::npos);
    ASSERT_NE(combinedReferenceScan, std::string::npos);
    EXPECT_LT(layoutValidation, invalidLayoutFilter);
    EXPECT_LT(invalidLayoutFilter, combinedReferenceScan);
    EXPECT_EQ(source.find("CountCandidateReferences(image, candidates);", combinedReferenceScan + 1),
              std::string::npos);
    EXPECT_EQ(source.find("CountCandidateReferences(image, candidate)"), std::string::npos);
}

TEST(RayReconstructionForceSourceTest, ScansTheMonolithicGameModuleBeforeDependencies) {
    const std::string source = ReadProjectSource("hook/main_ue5.cpp");

    const size_t mainModule = source.find("HMODULE mainModule = GetModuleHandleW(nullptr);");
    const size_t mainModuleScan = source.find("ScanModule(mainModule)", mainModule);
    const size_t dependencyLoop = source.find("for (HMODULE module : modules)", mainModule);
    ASSERT_NE(mainModule, std::string::npos);
    ASSERT_NE(mainModuleScan, std::string::npos);
    ASSERT_NE(dependencyLoop, std::string::npos);
    EXPECT_LT(mainModuleScan, dependencyLoop);
}
