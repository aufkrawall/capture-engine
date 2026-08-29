#include <gtest/gtest.h>

#include <filesystem>

#include "../hook/common/ngx_feature_lifecycle.h"
#include "source_fragment_reader.h"

TEST(NgxFeatureLifecycleTest, UsesTheOfficialNgxSuccessBitConvention) {
    EXPECT_TRUE(ce::ngx_lifecycle::IsSuccessfulResult(0x00000001u));
    EXPECT_TRUE(ce::ngx_lifecycle::IsSuccessfulResult(0x00012345u));
    EXPECT_FALSE(ce::ngx_lifecycle::IsSuccessfulResult(0xBAD00001u));
    EXPECT_FALSE(ce::ngx_lifecycle::IsSuccessfulResult(0xBADFFFFFu));
}

// Regression (session 20260811_222500): late-injected Talos is configured for
// 4x MFG but the NVNGX CreateFeature hook hardcoded the legacy FG feature IDs
// (9/0xB) to 2x, because the Streamline GetState path that keeps the
// multiplier fresh at startup is not hooked when sl.dlssg was already loaded.
// The multiplier resolution must accept the FrameGenerationMultiplier
// parameter on the legacy FG IDs exactly like the MFG (ID 18) branch.
TEST(NgxFeatureLifecycleTest, ResolvesDLSSFrameGenerationMultiplierFromParameter) {
    using ce::ngx_lifecycle::ResolveNVNGXFrameGenerationMultiplier;

    // No config override, no parameter -> standard FG default 2x.
    EXPECT_EQ(ResolveNVNGXFrameGenerationMultiplier(0, 0), 2);
    // THE regression: 4x MFG carried in the parameter (late inject, no
    // Streamline latch, no config override).
    EXPECT_EQ(ResolveNVNGXFrameGenerationMultiplier(0, 4), 4);
    EXPECT_EQ(ResolveNVNGXFrameGenerationMultiplier(0, 3), 3);
    EXPECT_EQ(ResolveNVNGXFrameGenerationMultiplier(0, 2), 2);
    // An explicit config override is authoritative even when the runtime later
    // stores another valid value in its live option object.
    EXPECT_EQ(ResolveNVNGXFrameGenerationMultiplier(4, 0), 4);
    EXPECT_EQ(ResolveNVNGXFrameGenerationMultiplier(4, 3), 4);
    // Invalid parameter values (absent/unreadable=0, out of range) are ignored.
    EXPECT_EQ(ResolveNVNGXFrameGenerationMultiplier(0, 1), 2);
    EXPECT_EQ(ResolveNVNGXFrameGenerationMultiplier(0, 5), 2);
    EXPECT_EQ(ResolveNVNGXFrameGenerationMultiplier(4, 5), 4);
}

TEST(NgxFeatureLifecycleTest, ModernGeneratedFrameCountOverridesLegacyMultiplier) {
    using ce::ngx_lifecycle::ResolveNVNGXObservedFrameGenerationMultiplier;

    EXPECT_EQ(ResolveNVNGXObservedFrameGenerationMultiplier(1, 4), 2);
    EXPECT_EQ(ResolveNVNGXObservedFrameGenerationMultiplier(2, 2), 3);
    EXPECT_EQ(ResolveNVNGXObservedFrameGenerationMultiplier(3, 2), 4);
    EXPECT_EQ(ResolveNVNGXObservedFrameGenerationMultiplier(0, 3), 3);
    EXPECT_EQ(ResolveNVNGXObservedFrameGenerationMultiplier(4, 4), 4);
    EXPECT_EQ(ResolveNVNGXObservedFrameGenerationMultiplier(0, 0), 0);
}

TEST(NgxFeatureLifecycleTest, EvaluateUsesLiveFactorUnlessAConfiguredOverrideWins) {
    using ce::ngx_lifecycle::ResolveNVNGXEvaluatedFrameGenerationMultiplier;

    EXPECT_EQ(ResolveNVNGXEvaluatedFrameGenerationMultiplier(0, 3, 2), 4);
    EXPECT_EQ(ResolveNVNGXEvaluatedFrameGenerationMultiplier(0, 2, 4), 3);
    EXPECT_EQ(ResolveNVNGXEvaluatedFrameGenerationMultiplier(0, 0, 4), 4);
    EXPECT_EQ(ResolveNVNGXEvaluatedFrameGenerationMultiplier(0, 0, 0), 0);
    EXPECT_EQ(ResolveNVNGXEvaluatedFrameGenerationMultiplier(2, 3, 4), 2);
    EXPECT_EQ(ResolveNVNGXEvaluatedFrameGenerationMultiplier(4, 1, 2), 4);
}

TEST(NgxFeatureLifecycleTest, ResolvesOfficialAndCompatibilityFrameGenerationParameterNames) {
    using ce::ngx_lifecycle::ResolveNVNGXFrameGenerationParameter;

    int value = 0;
    EXPECT_TRUE(ResolveNVNGXFrameGenerationParameter("DLSSG.MultiFrameCount", 3, value));
    EXPECT_EQ(value, 2);
    EXPECT_TRUE(ResolveNVNGXFrameGenerationParameter("MultiFrameCount", 4, value));
    EXPECT_EQ(value, 3);
    EXPECT_TRUE(ResolveNVNGXFrameGenerationParameter("FrameGenerationMultiplier", 2, value));
    EXPECT_EQ(value, 2);
    EXPECT_FALSE(ResolveNVNGXFrameGenerationParameter("DLSSG.MultiFrameIndex", 3, value));
    EXPECT_FALSE(ResolveNVNGXFrameGenerationParameter("DLSSG.MultiFrameCount", 5, value));
}

TEST(NgxFeatureLifecycleTest, NvngxHookUsesTheOfficialNamespacedMultiFrameCountKey) {
    const std::filesystem::path source =
        std::filesystem::current_path() / "hook" / "apis" / "nvngx_hook_internal.h";
    ASSERT_TRUE(std::filesystem::exists(source));

    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("NVSDK_NGX_DLSSG_Parameter_MultiFrameCount \"DLSSG.MultiFrameCount\""),
              std::string::npos);
    EXPECT_NE(text.find("NVSDK_NGX_DLSSG_Parameter_MultiFrameCount_Unscoped \"MultiFrameCount\""),
              std::string::npos);
}

TEST(NgxFeatureLifecycleTest, ParameterHooksWriteBothFrameGenerationContracts) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "nvngx_hook_params.cpp";
    const fs::path factorSource =
        fs::current_path() / "hook" / "apis" / "nvngx_hook_params_fg_factor.cpp";
    ASSERT_TRUE(fs::exists(source));
    ASSERT_TRUE(fs::exists(factorSource));

    const std::string text = ce::test_source::ReadLogicalSource(source) + ce::test_source::ReadFile(factorSource);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("DLSSFGMultiplierToGeneratedFrames(fgMultiplier)"), std::string::npos);
    EXPECT_NE(text.find("NVSDK_NGX_Parameter_FrameGenerationMultiplier"), std::string::npos);
    EXPECT_NE(text.find("NVSDK_NGX_DLSSG_Parameter_MultiFrameCount"), std::string::npos);
    EXPECT_NE(text.find("NVSDK_NGX_DLSSG_Parameter_MultiFrameCount_Unscoped"), std::string::npos);
    EXPECT_NE(text.find("ResolveAndApplyFGFactorForEvaluation"), std::string::npos);
    EXPECT_NE(text.find("RemixHook::ReassertFrameGenerationScheduleFromNgx("), std::string::npos);
    EXPECT_NE(text.find("ResolveNVNGXObservedFrameGenerationMultiplier"), std::string::npos)
        << "default telemetry must read the factor from the live EvaluateFeature parameters";
}

TEST(NgxFeatureLifecycleTest, EvaluateFeatureReassertsConfiguredFrameGenerationFactor) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "nvngx_hook_lifecycle.cpp";
    ASSERT_TRUE(fs::exists(source));

    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("IsFrameGenerationFeature(expectedFeature)"), std::string::npos);
    EXPECT_NE(text.find("ResolveAndApplyFGFactorForEvaluation"), std::string::npos);
    EXPECT_NE(text.find("SetDLSSFGMultiplier(evaluatedFGMultiplier)"), std::string::npos)
        << "successful evaluations must publish observed factors as well as configured overrides";
}

// Source invariant: both NVNGX CreateFeature FG branches (legacy IDs 9/0xB and
// MFG ID 18) must resolve the multiplier through the shared resolver instead of
// hardcoding 2x, so late injection reports the game's real MFG factor.
TEST(NgxFeatureLifecycleTest, CreateFeatureFGBranchesResolveTheMultiplierParameter) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "nvngx_hook_feature.cpp";
    ASSERT_TRUE(fs::exists(source));

    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty());

    // Both branches route through the resolver helper.
    EXPECT_NE(text.find("ResolveNVNGXFrameGenerationMultiplier("), std::string::npos);
    // The legacy FG branch must publish the resolved multiplier, not a
    // hardcoded 2x.
    EXPECT_NE(text.find("SetDLSSFGMultiplier(fgMultiplier)"), std::string::npos);
    // The resolver helper reads the parameter through the original vtable
    // getI/getUI, covering the legacy FrameGenerationMultiplier and current
    // namespaced DLSSG.MultiFrameCount contracts.
    EXPECT_NE(text.find("NVSDK_NGX_Parameter_FrameGenerationMultiplier"), std::string::npos);
    EXPECT_NE(text.find("NVSDK_NGX_DLSSG_Parameter_MultiFrameCount"), std::string::npos);
}

TEST(NgxFeatureLifecycleTest, TracksCreateFirstEvaluateAndRelease) {
    ce::ngx_lifecycle::FeatureHandleRegistry<4> registry;
    int handleStorage = 0;
    void* handle = &handleStorage;

    EXPECT_EQ(registry.RecordCreated(handle, 13), ce::ngx_lifecycle::RecordResult::kInserted);
    EXPECT_EQ(registry.FindFeature(handle), 13);
    EXPECT_FALSE(registry.HasEvaluatedFeature(13));

    const auto first = registry.MarkEvaluated(handle);
    EXPECT_TRUE(first.found);
    EXPECT_TRUE(first.firstEvaluation);
    EXPECT_EQ(first.feature, 13);
    EXPECT_TRUE(registry.HasEvaluatedFeature(13));

    const auto repeated = registry.MarkEvaluated(handle);
    EXPECT_TRUE(repeated.found);
    EXPECT_FALSE(repeated.firstEvaluation);

    const auto removed = registry.Remove(handle);
    EXPECT_TRUE(removed.found);
    EXPECT_TRUE(removed.wasEvaluated);
    EXPECT_EQ(removed.feature, 13);
    EXPECT_EQ(registry.FindFeature(handle), -1);
    EXPECT_FALSE(registry.HasEvaluatedFeature(13));
}

TEST(NgxFeatureLifecycleTest, KeepsOtherEvaluatedHandleActiveAcrossRelease) {
    ce::ngx_lifecycle::FeatureHandleRegistry<4> registry;
    int firstStorage = 0;
    int secondStorage = 0;

    ASSERT_EQ(registry.RecordCreated(&firstStorage, 1), ce::ngx_lifecycle::RecordResult::kInserted);
    ASSERT_EQ(registry.RecordCreated(&secondStorage, 1), ce::ngx_lifecycle::RecordResult::kInserted);
    ASSERT_TRUE(registry.MarkEvaluated(&firstStorage).found);
    ASSERT_TRUE(registry.MarkEvaluated(&secondStorage).found);

    EXPECT_TRUE(registry.Remove(&firstStorage).found);
    EXPECT_TRUE(registry.HasEvaluatedFeature(1));
    EXPECT_TRUE(registry.Remove(&secondStorage).found);
    EXPECT_FALSE(registry.HasEvaluatedFeature(1));
}

TEST(NgxFeatureLifecycleTest, TracksNonUpscalerHandlesWithoutPublishingThemAsUpscalers) {
    ce::ngx_lifecycle::FeatureHandleRegistry<4> registry;
    int frameGenerationHandle = 0;

    ASSERT_EQ(registry.RecordCreated(&frameGenerationHandle, 9), ce::ngx_lifecycle::RecordResult::kInserted);
    EXPECT_EQ(registry.FindFeature(&frameGenerationHandle), 9);

    const auto evaluation = registry.MarkEvaluated(&frameGenerationHandle);
    EXPECT_TRUE(evaluation.found);
    EXPECT_TRUE(evaluation.firstEvaluation);
    EXPECT_EQ(evaluation.feature, 9);
    EXPECT_TRUE(registry.HasEvaluatedFeature(9));
    EXPECT_FALSE(registry.HasEvaluatedFeature(1));
    EXPECT_FALSE(registry.HasEvaluatedFeature(13));
}

TEST(NgxFeatureLifecycleTest, UpdatesReusedHandleAndFailsClosedAtCapacity) {
    ce::ngx_lifecycle::FeatureHandleRegistry<1> registry;
    int firstStorage = 0;
    int secondStorage = 0;

    ASSERT_EQ(registry.RecordCreated(&firstStorage, 1), ce::ngx_lifecycle::RecordResult::kInserted);
    EXPECT_EQ(registry.RecordCreated(&firstStorage, 13), ce::ngx_lifecycle::RecordResult::kUpdated);
    EXPECT_EQ(registry.FindFeature(&firstStorage), 13);
    EXPECT_EQ(registry.RecordCreated(&secondStorage, 1), ce::ngx_lifecycle::RecordResult::kFull);
    EXPECT_EQ(registry.RecordCreated(nullptr, 1), ce::ngx_lifecycle::RecordResult::kInvalid);
}
