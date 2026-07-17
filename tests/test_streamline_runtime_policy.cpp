#include <gtest/gtest.h>

#include "../common/config.h"
#include "../hook/common/dx12_overlay_policy.h"
#include "../hook/common/streamline_runtime_policy.h"

namespace {

TEST(StreamlineRuntimePolicyTest, StreamlineModuleFeatureHookingRecognizesFeatureDlls) {
    EXPECT_TRUE(ce::streamline_runtime_policy::IsStreamlineModuleNameForFeatureHooking("sl.interposer.dll"));
    EXPECT_TRUE(ce::streamline_runtime_policy::IsStreamlineModuleNameForFeatureHooking("sl.common.dll"));
    EXPECT_TRUE(ce::streamline_runtime_policy::IsStreamlineModuleNameForFeatureHooking("sl.dlss_g.dll"));
    EXPECT_TRUE(ce::streamline_runtime_policy::IsStreamlineModuleNameForFeatureHooking("C:\\Game\\bin\\SL.Reflex.DLL"));

    EXPECT_FALSE(ce::streamline_runtime_policy::IsStreamlineModuleNameForFeatureHooking(nullptr));
    EXPECT_FALSE(ce::streamline_runtime_policy::IsStreamlineModuleNameForFeatureHooking("sl.dll"));
    EXPECT_FALSE(ce::streamline_runtime_policy::IsStreamlineModuleNameForFeatureHooking("sl.interposer.json"));
    EXPECT_FALSE(ce::streamline_runtime_policy::IsStreamlineModuleNameForFeatureHooking("_nvngx.dll"));
    EXPECT_FALSE(ce::streamline_runtime_policy::IsStreamlineModuleNameForFeatureHooking("nvngx_dlssg.dll"));
    EXPECT_FALSE(ce::streamline_runtime_policy::IsStreamlineModuleNameForFeatureHooking("notsl.dlss_g.dll"));
}

TEST(StreamlineRuntimePolicyTest, StreamlineCoreModuleRecognitionStaysNarrow) {
    EXPECT_TRUE(ce::streamline_runtime_policy::IsStreamlineCoreModuleName("C:\\Game\\sl.interposer.dll"));
    EXPECT_TRUE(ce::streamline_runtime_policy::IsStreamlineCoreModuleName("SL.COMMON.DLL"));

    EXPECT_FALSE(ce::streamline_runtime_policy::IsStreamlineCoreModuleName("sl.dlss_g.dll"));
    EXPECT_FALSE(ce::streamline_runtime_policy::IsStreamlineCoreModuleName("sl.reflex.dll"));
    EXPECT_FALSE(ce::streamline_runtime_policy::IsStreamlineCoreModuleName("nvngx_dlssg.dll"));
}

TEST(StreamlineRuntimePolicyTest, StreamlineModuleLoadInspectionIncludesFeatureDlls) {
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldInspectStreamlineModuleOnLoad("sl.interposer.dll"));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldInspectStreamlineModuleOnLoad("C:\\Game\\Plugins\\sl.dlss_g.dll"));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldInspectStreamlineModuleOnLoad("C:/Game/bin/sl.reflex.dll"));

    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldInspectStreamlineModuleOnLoad(nullptr));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldInspectStreamlineModuleOnLoad("_nvngx.dll"));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldInspectStreamlineModuleOnLoad("nvngx_dlssg.dll"));
}

TEST(StreamlineRuntimePolicyTest, StreamlineCoreExportsHookOnlyStableCoreModules) {
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldHookStreamlineCoreExportsOnLoad("sl.interposer.dll"));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldHookStreamlineCoreExportsOnLoad("C:\\Game\\SL.COMMON.DLL"));

    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldHookStreamlineCoreExportsOnLoad("sl.reflex.dll"));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldHookStreamlineCoreExportsOnLoad("sl.dlss_g.dll"));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldHookStreamlineCoreExportsOnLoad("sl.pcl.dll"));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldHookStreamlineCoreExportsOnLoad("nvngx_dlssg.dll"));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldHookStreamlineCoreExportsOnLoad(nullptr));
}

TEST(StreamlineRuntimePolicyTest, FeatureExportsHookOnlyTheirOwningFeatureModules) {
    EXPECT_TRUE(
        ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad("slDLSSGSetOptions", "sl.dlss_g.dll"));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad("slDLSSGGetState",
                                                                                       "C:\\Game\\SL.DLSS_G.DLL"));
    EXPECT_TRUE(
        ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad("slReflexSleep", "sl.reflex.dll"));
    EXPECT_TRUE(
        ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad("slReflexSetOptions", "sl.reflex.dll"));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad("slReflexSetConstants",
                                                                                       "sl.reflex.dll"));

    EXPECT_FALSE(
        ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad("slGetPluginFunction", "sl.reflex.dll"));
    EXPECT_FALSE(
        ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad("slDLSSGSetOptions", "sl.reflex.dll"));
    EXPECT_FALSE(
        ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad("slReflexSleep", "sl.dlss_g.dll"));
    EXPECT_FALSE(
        ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad("slReflexSetOptions", "sl.common.dll"));
}

TEST(StreamlineRuntimePolicyTest, ReturnedFeatureWrapperSubstitutionRequiresCallableOriginal) {
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldSubstituteReturnedStreamlineFeatureWrapper(true, false, true));

    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldSubstituteReturnedStreamlineFeatureWrapper(false, false, true));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldSubstituteReturnedStreamlineFeatureWrapper(true, true, true));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldSubstituteReturnedStreamlineFeatureWrapper(true, false, false));
}

TEST(StreamlineRuntimePolicyTest, StreamlineFeatureLookupIsDeferredDuringModuleLoad) {
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldDeferStreamlineFeatureLookupDuringModuleLoad(true));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldDeferStreamlineFeatureLookupDuringModuleLoad(false));
}

TEST(StreamlineRuntimePolicyTest, SavedOriginalForwardingRequiresLiveOwnerAddress) {
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldForwardSavedStreamlineOriginal(true, true));

    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldForwardSavedStreamlineOriginal(true, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldForwardSavedStreamlineOriginal(false, true));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldForwardSavedStreamlineOriginal(false, false));
}

TEST(StreamlineRuntimePolicyTest, LoadedModuleSnapshotRetryIsNarrow) {
    EXPECT_TRUE(ce::streamline_runtime_policy::IsRetryableLoadedModuleSnapshotError(24));

    EXPECT_FALSE(ce::streamline_runtime_policy::IsRetryableLoadedModuleSnapshotError(0));
    EXPECT_FALSE(ce::streamline_runtime_policy::IsRetryableLoadedModuleSnapshotError(5));
    EXPECT_FALSE(ce::streamline_runtime_policy::IsRetryableLoadedModuleSnapshotError(299));
}

TEST(StreamlineRuntimePolicyTest, RequestedOptionsEnableBuildsActiveRuntimeUpdate) {
    const auto update =
        ce::streamline_runtime_policy::BuildViewportRuntimeUpdateFromRequestedOptions(true, true, 1, 1, 3);

    EXPECT_TRUE(update.shouldUpdate);
    EXPECT_TRUE(update.active);
    EXPECT_EQ(2, update.multiplier);
    EXPECT_EQ(1u, update.generatedFrames);
    EXPECT_EQ(3u, update.capabilityMax);
}

TEST(StreamlineRuntimePolicyTest, RequestedOptionsOffBuildsInactiveRuntimeUpdate) {
    const auto update =
        ce::streamline_runtime_policy::BuildViewportRuntimeUpdateFromRequestedOptions(true, true, 0, 3, 3);

    EXPECT_TRUE(update.shouldUpdate);
    EXPECT_FALSE(update.active);
    EXPECT_EQ(0, update.multiplier);
    EXPECT_EQ(0u, update.generatedFrames);
}

TEST(StreamlineRuntimePolicyTest, SuppressedSetOptionsOffDoesNotApplyLocalRuntimeUpdate) {
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldApplyViewportRuntimeUpdateFromSetOptions(true, true));

    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldApplyViewportRuntimeUpdateFromSetOptions(true, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldApplyViewportRuntimeUpdateFromSetOptions(false, false));
}

TEST(StreamlineRuntimePolicyTest, ExplicitSetOptionsOffIsAuthoritativeAfterConfirmedPostSLRendering) {
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldTreatExplicitSetOptionsDisableAsAuthoritative(
        true, true, true, false, false, false, false));

    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldTreatExplicitSetOptionsDisableAsAuthoritative(
        true, false, true, false, false, false, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldTreatExplicitSetOptionsDisableAsAuthoritative(
        true, true, false, false, false, false, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldTreatExplicitSetOptionsDisableAsAuthoritative(
        true, true, true, true, false, false, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldTreatExplicitSetOptionsDisableAsAuthoritative(
        true, true, true, false, true, false, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldTreatExplicitSetOptionsDisableAsAuthoritative(
        true, true, true, false, false, true, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldTreatExplicitSetOptionsDisableAsAuthoritative(
        true, true, true, false, false, false, true));

    // A GetState OFF edge can be accepted before its matching SetOptions call.
    // The runtime call remains authoritative while prior PostSL startup proof is
    // unwinding, otherwise Streamline never receives the user's disable request.
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldTreatExplicitSetOptionsDisableAsAuthoritative(
        true, true, false, true, true, true, true, true));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldTreatExplicitSetOptionsDisableAsAuthoritative(
        false, true, false, true, true, true, true, true));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldTreatExplicitSetOptionsDisableAsAuthoritative(
        true, false, false, true, true, true, true, true));
}

TEST(StreamlineRuntimePolicyTest, AcceptedGetStateOffWaitsForMatchingSetOptionsDisable) {
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldLatchAcceptedRuntimeOffAwaitingSetOptions(true, false, true));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldLatchAcceptedRuntimeOffAwaitingSetOptions(false, false, true));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldLatchAcceptedRuntimeOffAwaitingSetOptions(true, true, true));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldLatchAcceptedRuntimeOffAwaitingSetOptions(true, false, false));
}

TEST(StreamlineRuntimePolicyTest, SuccessfulSetOptionsDisableClearsCachedStreamlineViewports) {
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldClearAllViewportRuntimeStatesForSetOptionsDisable(true, false, 0));

    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldClearAllViewportRuntimeStatesForSetOptionsDisable(true, true, 0));
    EXPECT_FALSE(
        ce::streamline_runtime_policy::ShouldClearAllViewportRuntimeStatesForSetOptionsDisable(false, false, 0));
    EXPECT_FALSE(
        ce::streamline_runtime_policy::ShouldClearAllViewportRuntimeStatesForSetOptionsDisable(true, false, 1));
}

TEST(StreamlineRuntimePolicyTest, RequestedOptionsFallbacksToTwoXForInvalidGeneratedFrameCount) {
    const auto update =
        ce::streamline_runtime_policy::BuildViewportRuntimeUpdateFromRequestedOptions(true, true, 2, 0, 1);

    EXPECT_TRUE(update.shouldUpdate);
    EXPECT_TRUE(update.active);
    EXPECT_EQ(2, update.multiplier);
    EXPECT_EQ(1u, update.generatedFrames);
}

TEST(StreamlineRuntimePolicyTest, GetStateIgnoresEnableRequestsWithoutRuntimeEvidence) {
    const auto update =
        ce::streamline_runtime_policy::BuildViewportRuntimeUpdateFromGetState(true, true, false, false, false, 1, 1, 3);

    EXPECT_FALSE(update.shouldUpdate);
    EXPECT_FALSE(update.active);
}

TEST(StreamlineRuntimePolicyTest, GetStateAllowsEnableWithRuntimeEvidence) {
    const auto update =
        ce::streamline_runtime_policy::BuildViewportRuntimeUpdateFromGetState(true, true, false, true, false, 1, 3, 3);

    EXPECT_TRUE(update.shouldUpdate);
    EXPECT_TRUE(update.active);
    EXPECT_EQ(4, update.multiplier);
    EXPECT_EQ(3u, update.generatedFrames);
}

TEST(StreamlineRuntimePolicyTest, GetStateAllowsDisableWithoutRuntimeEvidence) {
    const auto update =
        ce::streamline_runtime_policy::BuildViewportRuntimeUpdateFromGetState(true, true, true, false, false, 0, 1, 3);

    EXPECT_TRUE(update.shouldUpdate);
    EXPECT_FALSE(update.active);
    EXPECT_EQ(0, update.multiplier);
    EXPECT_EQ(0u, update.generatedFrames);
}

TEST(StreamlineRuntimePolicyTest, GetStateKeepsKnownActiveViewportWithoutFreshFenceEvidence) {
    const auto update =
        ce::streamline_runtime_policy::BuildViewportRuntimeUpdateFromGetState(true, true, true, false, false, 1, 1, 3);

    EXPECT_TRUE(update.shouldUpdate);
    EXPECT_TRUE(update.active);
    EXPECT_EQ(2, update.multiplier);
    EXPECT_EQ(1u, update.generatedFrames);
}

TEST(StreamlineRuntimePolicyTest, FailedOrOptionlessCallsDoNotUpdateRuntimeState) {
    const auto failedUpdate =
        ce::streamline_runtime_policy::BuildViewportRuntimeUpdateFromRequestedOptions(false, true, 1, 1, 3);
    const auto optionlessUpdate =
        ce::streamline_runtime_policy::BuildViewportRuntimeUpdateFromGetState(true, false, false, true, false, 1, 1, 3);

    EXPECT_FALSE(failedUpdate.shouldUpdate);
    EXPECT_FALSE(optionlessUpdate.shouldUpdate);
}

TEST(StreamlineRuntimePolicyTest, GetStateSuppressesFreshActivationDuringRecentAuthoritativeTakeover) {
    const auto update =
        ce::streamline_runtime_policy::BuildViewportRuntimeUpdateFromGetState(true, true, false, true, true, 1, 1, 3);

    EXPECT_FALSE(update.shouldUpdate);
    EXPECT_FALSE(update.active);
}

TEST(StreamlineRuntimePolicyTest, GetStateSuppressionDoesNotDisableAlreadyActiveViewport) {
    const auto update =
        ce::streamline_runtime_policy::BuildViewportRuntimeUpdateFromGetState(true, true, true, true, true, 1, 1, 3);

    EXPECT_TRUE(update.shouldUpdate);
    EXPECT_TRUE(update.active);
    EXPECT_EQ(2, update.multiplier);
    EXPECT_EQ(1u, update.generatedFrames);
}

TEST(StreamlineRuntimePolicyTest, EvaluateGetStateSuppressionBlocksFreshActivationEvenWithFenceEvidence) {
    const auto evaluation = ce::streamline_runtime_policy::EvaluateViewportRuntimeUpdateFromGetState(
        true, true, false, true, true, 1, 1, 3);

    EXPECT_FALSE(evaluation.update.shouldUpdate);
    EXPECT_FALSE(evaluation.update.active);
    EXPECT_TRUE(evaluation.suppressedFreshActivation);
}

TEST(StreamlineRuntimePolicyTest, AuthoritativeGetStateDisableClearsCachedStreamlineViewports) {
    EXPECT_TRUE(
        ce::streamline_runtime_policy::ShouldClearAllViewportRuntimeStatesForGetStateDisable(true, true, true, 0, 5));

    EXPECT_FALSE(
        ce::streamline_runtime_policy::ShouldClearAllViewportRuntimeStatesForGetStateDisable(true, true, false, 0, 5));
    EXPECT_FALSE(
        ce::streamline_runtime_policy::ShouldClearAllViewportRuntimeStatesForGetStateDisable(true, true, true, 1, 5));
    EXPECT_FALSE(
        ce::streamline_runtime_policy::ShouldClearAllViewportRuntimeStatesForGetStateDisable(true, true, true, 0, 0));
    EXPECT_FALSE(
        ce::streamline_runtime_policy::ShouldClearAllViewportRuntimeStatesForGetStateDisable(false, true, true, 0, 5));
}

TEST(StreamlineRuntimePolicyTest, FreshGetStateActivationSuppressedWhileRuntimeStillInactive) {
    using ce::fg_runtime::RuntimeMode;

    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldSuppressFreshGetStateActivationWhileRuntimeInactive(
        false, true, RuntimeMode::kOff));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldSuppressFreshGetStateActivationWhileRuntimeInactive(
        true, false, RuntimeMode::kOff));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldSuppressFreshGetStateActivationWhileRuntimeInactive(
        false, true, RuntimeMode::kStreamlineNoFG));

    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldSuppressFreshGetStateActivationWhileRuntimeInactive(
        false, false, RuntimeMode::kOff));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldSuppressFreshGetStateActivationWhileRuntimeInactive(
        false, true, RuntimeMode::kDLSSFG));
}

TEST(StreamlineRuntimePolicyTest, FreshGetStateActivationStaysSuppressedDuringUnsafePostFSRComeback) {
    using ce::fg_runtime::RuntimeMode;

    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldSuppressFreshGetStateActivationDuringUnsafePostFSRComeback(
        true, false, RuntimeMode::kOff));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldSuppressFreshGetStateActivationDuringUnsafePostFSRComeback(
        true, false, RuntimeMode::kStreamlineNoFG));

    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldSuppressFreshGetStateActivationDuringUnsafePostFSRComeback(
        false, false, RuntimeMode::kOff));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldSuppressFreshGetStateActivationDuringUnsafePostFSRComeback(
        true, true, RuntimeMode::kOff));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldSuppressFreshGetStateActivationDuringUnsafePostFSRComeback(
        true, false, RuntimeMode::kDLSSFG));
}

TEST(StreamlineRuntimePolicyTest, StartupTransitionWindowOnlyRearmsOnFreshActiveSignal) {
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldArmStartupTransitionWindowOnFreshActiveSignal(true, false));

    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldArmStartupTransitionWindowOnFreshActiveSignal(true, true));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldArmStartupTransitionWindowOnFreshActiveSignal(false, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldArmStartupTransitionWindowOnFreshActiveSignal(false, true));
}

TEST(StreamlineRuntimePolicyTest, StartupWindowOffExtensionLatchOnlyPrimesOnFreshActiveEdge) {
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldPrimeStartupWindowOffExtensionLatch(true, true));

    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldPrimeStartupWindowOffExtensionLatch(true, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldPrimeStartupWindowOffExtensionLatch(false, true));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldPrimeStartupWindowOffExtensionLatch(false, false));
}

TEST(StreamlineRuntimePolicyTest, DeferredStartupWindowOffKeepsEffectiveDlssSignalAndMultiplier) {
    const auto update = ce::streamline_runtime_policy::ResolveCombinedRuntimeSignalUpdate(false, true, true, 2);

    EXPECT_TRUE(update.deferredOffDuringStartupWindow);
    EXPECT_TRUE(update.effectiveActive);
    EXPECT_EQ(2, update.effectiveMultiplier);
    EXPECT_FALSE(update.freshActivationEdge);
    EXPECT_TRUE(update.shouldExtendStartupTransitionWindow);
}

TEST(StreamlineRuntimePolicyTest, NonDeferredOffClearsEffectiveDlssSignalAndMultiplier) {
    const auto update = ce::streamline_runtime_policy::ResolveCombinedRuntimeSignalUpdate(false, false, true, 2);

    EXPECT_FALSE(update.deferredOffDuringStartupWindow);
    EXPECT_FALSE(update.effectiveActive);
    EXPECT_EQ(0, update.effectiveMultiplier);
    EXPECT_FALSE(update.freshActivationEdge);
    EXPECT_FALSE(update.shouldExtendStartupTransitionWindow);
}

TEST(StreamlineRuntimePolicyTest, FreshActivationEdgeOnlyTracksOffToOnTransitions) {
    const auto freshOn = ce::streamline_runtime_policy::ResolveCombinedRuntimeSignalUpdate(true, false, false, 4);
    const auto steadyOn = ce::streamline_runtime_policy::ResolveCombinedRuntimeSignalUpdate(true, false, true, 4);
    const auto deferredOff = ce::streamline_runtime_policy::ResolveCombinedRuntimeSignalUpdate(false, true, true, 4);

    EXPECT_TRUE(freshOn.freshActivationEdge);
    EXPECT_TRUE(freshOn.effectiveActive);

    EXPECT_FALSE(steadyOn.freshActivationEdge);
    EXPECT_TRUE(steadyOn.effectiveActive);
    EXPECT_FALSE(steadyOn.shouldExtendStartupTransitionWindow);

    EXPECT_FALSE(deferredOff.freshActivationEdge);
    EXPECT_TRUE(deferredOff.effectiveActive);
    EXPECT_TRUE(deferredOff.shouldExtendStartupTransitionWindow);
}

TEST(StreamlineRuntimePolicyTest, ExplicitSetOptionsComebackProofPersistsAcrossDeferredStartupOffChurn) {
    EXPECT_TRUE(
        ce::streamline_runtime_policy::ResolveCurrentComebackExplicitSetOptionsActivation(false, true, true, true));

    const auto deferredOff = ce::streamline_runtime_policy::ResolveCombinedRuntimeSignalUpdate(false, true, true, 4);
    EXPECT_TRUE(ce::streamline_runtime_policy::ResolveCurrentComebackExplicitSetOptionsActivation(
        true, deferredOff.effectiveActive, deferredOff.freshActivationEdge, false));

    const auto realOff = ce::streamline_runtime_policy::ResolveCombinedRuntimeSignalUpdate(false, false, true, 4);
    EXPECT_FALSE(ce::streamline_runtime_policy::ResolveCurrentComebackExplicitSetOptionsActivation(
        true, realOff.effectiveActive, realOff.freshActivationEdge, false));

    const auto getStateComeback =
        ce::streamline_runtime_policy::ResolveCombinedRuntimeSignalUpdate(true, false, false, 4);
    EXPECT_FALSE(ce::streamline_runtime_policy::ResolveCurrentComebackExplicitSetOptionsActivation(
        false, getStateComeback.effectiveActive, getStateComeback.freshActivationEdge, false));
}

TEST(StreamlineRuntimePolicyTest, ExplicitSetOptionsEnableUpgradesAlreadyLiveGetStateComeback) {
    const auto getStateComeback =
        ce::streamline_runtime_policy::ResolveCombinedRuntimeSignalUpdate(true, false, false, 4);
    ASSERT_TRUE(getStateComeback.effectiveActive);
    ASSERT_TRUE(getStateComeback.freshActivationEdge);

    const bool getStateOnly = ce::streamline_runtime_policy::ResolveCurrentComebackExplicitSetOptionsActivation(
        false, getStateComeback.effectiveActive, getStateComeback.freshActivationEdge, false);
    EXPECT_FALSE(getStateOnly);

    const bool upgradedToExplicit = ce::streamline_runtime_policy::ResolveCurrentComebackExplicitSetOptionsActivation(
        getStateOnly, true, false, true);
    EXPECT_TRUE(upgradedToExplicit);
}

TEST(StreamlineRuntimePolicyTest, DeferredStartupWindowOffDoesNotExtendWithoutExistingActiveSignal) {
    const auto deferredWithoutActive =
        ce::streamline_runtime_policy::ResolveCombinedRuntimeSignalUpdate(false, true, false, 4);

    EXPECT_TRUE(deferredWithoutActive.deferredOffDuringStartupWindow);
    EXPECT_FALSE(deferredWithoutActive.effectiveActive);
    EXPECT_EQ(0, deferredWithoutActive.effectiveMultiplier);
    EXPECT_FALSE(deferredWithoutActive.shouldExtendStartupTransitionWindow);
}

TEST(StreamlineRuntimePolicyTest, PrepareForStreamlineEnableBeforeOriginalCallOnlyRunsForFsrOwnedHandoff) {
    EXPECT_TRUE(
        ce::streamline_runtime_policy::ShouldPrepareForStreamlineEnableBeforeOriginalCall(true, true, false, true));
    EXPECT_TRUE(
        ce::streamline_runtime_policy::ShouldPrepareForStreamlineEnableBeforeOriginalCall(true, false, true, true));

    EXPECT_FALSE(
        ce::streamline_runtime_policy::ShouldPrepareForStreamlineEnableBeforeOriginalCall(false, true, false, true));
    EXPECT_FALSE(
        ce::streamline_runtime_policy::ShouldPrepareForStreamlineEnableBeforeOriginalCall(true, false, false, true));
    EXPECT_FALSE(
        ce::streamline_runtime_policy::ShouldPrepareForStreamlineEnableBeforeOriginalCall(true, true, false, false));
}

TEST(StreamlineRuntimePolicyTest, ReflexActivationOnlyRequestsPrepareDuringFsrOwnedHandoff) {
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldRequestStreamlineEnablePreparationOnReflexActivation(true, true,
                                                                                                          false, true));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldRequestStreamlineEnablePreparationOnReflexActivation(true, false,
                                                                                                          true, true));

    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldRequestStreamlineEnablePreparationOnReflexActivation(
        false, true, false, true));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldRequestStreamlineEnablePreparationOnReflexActivation(
        true, false, false, true));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldRequestStreamlineEnablePreparationOnReflexActivation(
        true, true, false, false));
}

TEST(StreamlineRuntimePolicyTest, ReflexFrameLimitIsPacingSignalEvenWhenLowLatencyModeIsOff) {
    EXPECT_FALSE(ce::streamline_runtime_policy::IsStreamlineReflexPacingSignalActive(0, 0));
    EXPECT_TRUE(ce::streamline_runtime_policy::IsStreamlineReflexPacingSignalActive(1, 0));
    EXPECT_TRUE(ce::streamline_runtime_policy::IsStreamlineReflexPacingSignalActive(2, 0));
    EXPECT_TRUE(ce::streamline_runtime_policy::IsStreamlineReflexPacingSignalActive(0, 8333));
}

TEST(StreamlineRuntimePolicyTest, ReflexFrameLimitOverrideFollowsConfiguredTarget) {
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldOverrideStreamlineReflexFrameLimit(0));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldOverrideStreamlineReflexFrameLimit(8333));
}

TEST(StreamlineRuntimePolicyTest, ReflexFrameLimitForwardingKeepsIncomingSignalOwnershipSeparate) {
    const auto noOverride = ce::streamline_runtime_policy::ResolveStreamlineReflexFrameLimitForwarding(0, 0);
    EXPECT_EQ(0u, noOverride.frameLimitUs);
    EXPECT_FALSE(noOverride.overrideApplied);

    const auto overridden = ce::streamline_runtime_policy::ResolveStreamlineReflexFrameLimitForwarding(0, 8333);
    EXPECT_EQ(8333u, overridden.frameLimitUs);
    EXPECT_TRUE(overridden.overrideApplied);

    EXPECT_FALSE(ce::streamline_runtime_policy::IsStreamlineReflexPacingSignalActive(0, 0));
    EXPECT_TRUE(ce::streamline_runtime_policy::IsStreamlineReflexFrameLimitActive(overridden.frameLimitUs));
}

TEST(StreamlineRuntimePolicyTest, ConfirmedReflexOffArmsAuthoritativeDLSSSuspendAfterFirstPostSLRender) {
    using ce::streamline_runtime_policy::ShouldArmConfirmedDLSSReflexSuspendIntent;

    EXPECT_TRUE(ShouldArmConfirmedDLSSReflexSuspendIntent(true, true, true, true, false, false));

    EXPECT_FALSE(ShouldArmConfirmedDLSSReflexSuspendIntent(false, true, true, true, false, false));
    EXPECT_FALSE(ShouldArmConfirmedDLSSReflexSuspendIntent(true, false, true, true, false, false));
    EXPECT_FALSE(ShouldArmConfirmedDLSSReflexSuspendIntent(true, true, false, true, false, false));
    EXPECT_FALSE(ShouldArmConfirmedDLSSReflexSuspendIntent(true, true, true, false, false, false));
    EXPECT_FALSE(ShouldArmConfirmedDLSSReflexSuspendIntent(true, true, true, true, true, false));
    EXPECT_FALSE(ShouldArmConfirmedDLSSReflexSuspendIntent(true, true, true, true, false, true));
}

TEST(StreamlineRuntimePolicyTest, FirstConfirmedPostFSRReflexSuspendOverridesStartupOffChurnGuard) {
    using ce::streamline_runtime_policy::ResolveCombinedRuntimeSignalUpdate;
    using ce::streamline_runtime_policy::ShouldAcceptInactiveStreamlineSignalAfterConfirmedReflexSuspend;
    using ce::streamline_runtime_policy::ShouldArmConfirmedDLSSReflexSuspendIntent;
    using ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedStreamlineComeback;

    // GTA session 20260717_235919 switched FSR FG -> DLSS FG in the menu. After
    // the first confirmed PostSL render, GTA turned Reflex OFF and requested
    // DLSSG OFF while CE's startup window was still active. The generic guard
    // must remain true for unrelated stale churn, but the paired game-owned
    // Reflex suspend makes this inactive edge authoritative.
    const bool startupPolicyWouldDefer = ShouldKeepOffChurnDeferredForStartupProtectedStreamlineComeback(
        true, true, true, true, false, false, true, true, true);
    ASSERT_TRUE(startupPolicyWouldDefer);

    const bool suspendIntent =
        ShouldArmConfirmedDLSSReflexSuspendIntent(true, true, true, true, false, false);
    ASSERT_TRUE(suspendIntent);
    const bool reflexSuspendIsAuthoritative =
        ShouldAcceptInactiveStreamlineSignalAfterConfirmedReflexSuspend(suspendIntent, true, true);
    const auto update = ResolveCombinedRuntimeSignalUpdate(
        false, startupPolicyWouldDefer && !reflexSuspendIsAuthoritative, true, 0);
    EXPECT_FALSE(update.deferredOffDuringStartupWindow);
    EXPECT_FALSE(update.effectiveActive);
    EXPECT_EQ(0, update.effectiveMultiplier);
}

TEST(StreamlineRuntimePolicyTest, ConfirmedReflexSuspendBypassesOnlyNextActiveToInactiveRuntimeEdge) {
    using ce::streamline_runtime_policy::ShouldAcceptInactiveStreamlineSignalAfterConfirmedReflexSuspend;

    EXPECT_TRUE(ShouldAcceptInactiveStreamlineSignalAfterConfirmedReflexSuspend(true, true, true));
    EXPECT_FALSE(ShouldAcceptInactiveStreamlineSignalAfterConfirmedReflexSuspend(false, true, true));
    EXPECT_FALSE(ShouldAcceptInactiveStreamlineSignalAfterConfirmedReflexSuspend(true, false, true));
    EXPECT_FALSE(ShouldAcceptInactiveStreamlineSignalAfterConfirmedReflexSuspend(true, true, false));

    // A configured manual cap can still override the forwarded frame interval;
    // suspend ownership follows the game's incoming Reflex OFF edge separately.
    const auto manualCap = ce::streamline_runtime_policy::ResolveStreamlineReflexFrameLimitForwarding(0, 16666);
    EXPECT_TRUE(manualCap.overrideApplied);
    EXPECT_EQ(16666u, manualCap.frameLimitUs);
    EXPECT_TRUE(ShouldAcceptInactiveStreamlineSignalAfterConfirmedReflexSuspend(true, true, true));
}

TEST(StreamlineRuntimePolicyTest, RepeatedMenuSuspendCannotBeReclassifiedAsStartupOffChurn) {
    using ce::streamline_runtime_policy::ResolveCombinedRuntimeSignalUpdate;
    using ce::streamline_runtime_policy::ShouldAcceptInactiveStreamlineSignalAfterConfirmedReflexSuspend;
    using ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedStreamlineComeback;

    // Reproduce session 20260715_141520 after a successful menu resume: the
    // previous startup OFF-churn latch lacks fresh active proof, so the generic
    // stale-startup policy alone would incorrectly keep DLSS-G ON forever.
    const bool staleStartupPolicyWouldDefer = ShouldKeepOffChurnDeferredForStartupProtectedStreamlineComeback(
        false, false, true, false, false, false, true, false, true);
    ASSERT_TRUE(staleStartupPolicyWouldDefer);

    const bool reflexSuspendIsAuthoritative =
        ShouldAcceptInactiveStreamlineSignalAfterConfirmedReflexSuspend(true, true, true);
    const auto update = ResolveCombinedRuntimeSignalUpdate(
        false, staleStartupPolicyWouldDefer && !reflexSuspendIsAuthoritative, true, 0);
    EXPECT_FALSE(update.deferredOffDuringStartupWindow);
    EXPECT_FALSE(update.effectiveActive);
    EXPECT_EQ(0, update.effectiveMultiplier);
}

TEST(StreamlineRuntimePolicyTest, SuppressSetOptionsOffDuringStartupTransitionWindow) {
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldSuppressSetOptionsOffDuringStartupTransitionWindow(true, true));

    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldSuppressSetOptionsOffDuringStartupTransitionWindow(true, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldSuppressSetOptionsOffDuringStartupTransitionWindow(false, true));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldSuppressSetOptionsOffDuringStartupTransitionWindow(false, false));
}

TEST(StreamlineRuntimePolicyTest, StartupProtectedPostFSRComebackKeepsOffChurnDeferredWhileHalfArmed) {
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedPostFSRComeback(
        true, true, false, false, false, false, false, false, false));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedPostFSRComeback(
        false, true, true, false, true, false, false, false, false));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedPostFSRComeback(
        false, true, true, false, false, true, false, false, false));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedPostFSRComeback(
        false, true, true, false, false, false, true, true, false));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedPostFSRComeback(
        false, true, false, true, true, false, false, false, false));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedPostFSRComeback(
        false, true, false, true, false, true, false, false, false));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedPostFSRComeback(
        false, true, false, true, false, false, true, true, false));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedPostFSRComeback(
        false, true, true, false, false, false, true, false, true));

    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedPostFSRComeback(
        false, false, true, false, true, false, false, false, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedPostFSRComeback(
        false, true, false, false, true, false, false, false, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedPostFSRComeback(
        false, true, true, false, false, false, true, false, false));
}

TEST(StreamlineRuntimePolicyTest, StartupProtectedPureDLSSComebackKeepsOffChurnDeferredWhileHalfArmed) {
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedPureDLSSComeback(
        true, false, false, false, false, false, false, false));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedPureDLSSComeback(
        false, false, true, true, false, false, false, false));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedPureDLSSComeback(
        false, false, true, false, true, false, false, false));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedPureDLSSComeback(
        false, false, true, false, false, true, true, false));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedPureDLSSComeback(
        false, false, true, false, false, true, false, true));

    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedPureDLSSComeback(
        false, true, true, true, false, false, false, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedPureDLSSComeback(
        false, false, false, true, false, false, false, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedPureDLSSComeback(
        false, false, true, false, false, true, false, false));
}

TEST(StreamlineRuntimePolicyTest, StartupProtectedPostFSRComebackDropsStaleSuppressedOffChurnOnceActive) {
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldDropSuppressedOffChurnForStartupProtectedPostFSRComeback(
        true, true, false, true, false, false));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldDropSuppressedOffChurnForStartupProtectedPostFSRComeback(
        true, false, true, true, false, false));

    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldDropSuppressedOffChurnForStartupProtectedPostFSRComeback(
        false, true, false, true, false, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldDropSuppressedOffChurnForStartupProtectedPostFSRComeback(
        true, false, false, true, false, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldDropSuppressedOffChurnForStartupProtectedPostFSRComeback(
        true, true, false, false, false, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldDropSuppressedOffChurnForStartupProtectedPostFSRComeback(
        true, true, false, true, true, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldDropSuppressedOffChurnForStartupProtectedPostFSRComeback(
        true, true, false, true, false, true));
}

TEST(StreamlineRuntimePolicyTest, StartupProtectedPureDLSSComebackDropsStaleSuppressedOffChurnOnceActive) {
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldDropSuppressedOffChurnForStartupProtectedPureDLSSComeback(
        false, true, true, false, false));

    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldDropSuppressedOffChurnForStartupProtectedPureDLSSComeback(
        true, true, true, false, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldDropSuppressedOffChurnForStartupProtectedPureDLSSComeback(
        false, false, true, false, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldDropSuppressedOffChurnForStartupProtectedPureDLSSComeback(
        false, true, false, false, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldDropSuppressedOffChurnForStartupProtectedPureDLSSComeback(
        false, true, true, true, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldDropSuppressedOffChurnForStartupProtectedPureDLSSComeback(
        false, true, true, false, true));
}

TEST(StreamlineRuntimePolicyTest, StartupProtectedStreamlineComebackIncludesPureDLSSProtection) {
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedStreamlineComeback(
        false, false, true, false, true, false, false, false, false));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldDropSuppressedOffChurnForStartupProtectedStreamlineComeback(
        false, true, false, true, false, false));
}

TEST(StreamlineRuntimePolicyTest, StartupProtectedPureDLSSComebackStaysDeferredDuringPostSettlingStabilization) {
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedStreamlineComeback(
        false, false, true, false, false, false, true, false, true));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldDropSuppressedOffChurnForStartupProtectedStreamlineComeback(
        false, true, false, true, false, true));
}

TEST(StreamlineRuntimePolicyTest, GetStateWarmupProtectionKeepsPureDLSSOffChurnDeferred) {
    const bool getStateWarmupProtection =
        ce::dx12_overlay_policy::ShouldDeferGetStateOffDuringConfirmedPostSLWarmup(true, 13);
    const bool staleOffWarmupProtection =
        ce::dx12_overlay_policy::ShouldDeferStaleOffDuringConfirmedPostSLWarmup(true, 13);

    EXPECT_TRUE(getStateWarmupProtection);
    EXPECT_TRUE(staleOffWarmupProtection);
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedStreamlineComeback(
        false, false, true, false, false, false, true, false, staleOffWarmupProtection));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldDropSuppressedOffChurnForStartupProtectedStreamlineComeback(
        false, true, false, true, false, staleOffWarmupProtection));
}

TEST(StreamlineRuntimePolicyTest, StartupProtectedOffChurnWaitsForActiveProofAfterPostSLConfirmation) {
    EXPECT_EQ(3u, ce::streamline_runtime_policy::GetStartupProtectedOffChurnActiveProofUpdateThreshold());

    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldKeepStartupProtectedOffChurnDeferredUntilActiveProof(true, 0, true,
                                                                                                          true, false));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldKeepStartupProtectedOffChurnDeferredUntilActiveProof(true, 2, true,
                                                                                                          true, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldKeepStartupProtectedOffChurnDeferredUntilActiveProof(
        true, 3, true, true, false));

    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldKeepStartupProtectedOffChurnDeferredUntilActiveProof(
        false, 0, true, true, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldKeepStartupProtectedOffChurnDeferredUntilActiveProof(
        true, 0, false, true, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldKeepStartupProtectedOffChurnDeferredUntilActiveProof(
        true, 0, true, false, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldKeepStartupProtectedOffChurnDeferredUntilActiveProof(
        true, 0, true, true, true));
}

TEST(StreamlineRuntimePolicyTest, ActivatedUnconfirmedStreamlineResumeAcceptsRealSuspendOffEdge) {
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldAcceptOffSignalDuringActivatedUnconfirmedStreamlineResume(
        true, false, true, true, true, true, false, false, false));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldAcceptOffSignalDuringActivatedUnconfirmedStreamlineResume(
        true, false, true, false, true, true, false, false, false));

    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldAcceptOffSignalDuringActivatedUnconfirmedStreamlineResume(
        false, false, true, true, true, true, false, false, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldAcceptOffSignalDuringActivatedUnconfirmedStreamlineResume(
        true, true, true, true, true, true, false, false, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldAcceptOffSignalDuringActivatedUnconfirmedStreamlineResume(
        true, false, false, true, true, true, false, false, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldAcceptOffSignalDuringActivatedUnconfirmedStreamlineResume(
        true, false, true, true, false, true, false, false, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldAcceptOffSignalDuringActivatedUnconfirmedStreamlineResume(
        true, false, true, true, true, false, false, false, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldAcceptOffSignalDuringActivatedUnconfirmedStreamlineResume(
        true, false, true, true, true, true, true, false, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldAcceptOffSignalDuringActivatedUnconfirmedStreamlineResume(
        true, false, true, true, true, true, false, true, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldAcceptOffSignalDuringActivatedUnconfirmedStreamlineResume(
        true, false, true, true, true, true, false, false, true));
}

TEST(StreamlineRuntimePolicyTest,
     CombinedRuntimeStateDefersHalfArmedStartupProtectedPostFSRComebackOffAfterWindowExpiry) {
    const bool deferOff = ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedPostFSRComeback(
        false, true, false, true, true, false, false, false, false);
    const auto update = ce::streamline_runtime_policy::ResolveCombinedRuntimeSignalUpdate(false, deferOff, true, 2);

    EXPECT_TRUE(update.deferredOffDuringStartupWindow);
    EXPECT_TRUE(update.effectiveActive);
    EXPECT_EQ(2, update.effectiveMultiplier);
}

TEST(StreamlineRuntimePolicyTest, DirectPostSLCallbackTriggerStopsAfterStartupCallbackEnters) {
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldTriggerDirectPostSLCallbackAfterStartupWindowExpiry(true, false));

    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldTriggerDirectPostSLCallbackAfterStartupWindowExpiry(true, true));
    EXPECT_FALSE(
        ce::streamline_runtime_policy::ShouldTriggerDirectPostSLCallbackAfterStartupWindowExpiry(false, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldTriggerDirectPostSLCallbackAfterStartupWindowExpiry(false, true));
}

TEST(StreamlineRuntimePolicyTest, ObserverOnlyActivationClearsRecentTeardownGraceAndResetsHeuristics) {
    const auto cleanup =
        ce::streamline_runtime_policy::ResolveObserverOnlyHeuristicCleanupForStreamlineSignalTransition(true);

    EXPECT_TRUE(cleanup.clearRecentTeardownGrace);
    EXPECT_FALSE(cleanup.seedRecentTeardownGrace);
    EXPECT_TRUE(cleanup.resetQueueChangeHeuristic);
    EXPECT_TRUE(cleanup.clearHeuristicFSR);
    EXPECT_TRUE(cleanup.clearNvidiaSmoothMotion);
}

TEST(StreamlineRuntimePolicyTest, ObserverOnlyDeactivationSeedsRecentTeardownGraceAndResetsHeuristics) {
    const auto cleanup =
        ce::streamline_runtime_policy::ResolveObserverOnlyHeuristicCleanupForStreamlineSignalTransition(false);

    EXPECT_FALSE(cleanup.clearRecentTeardownGrace);
    EXPECT_TRUE(cleanup.seedRecentTeardownGrace);
    EXPECT_TRUE(cleanup.resetQueueChangeHeuristic);
    EXPECT_TRUE(cleanup.clearHeuristicFSR);
    EXPECT_TRUE(cleanup.clearNvidiaSmoothMotion);
}

TEST(StreamlineRuntimePolicyTest, ObserverStartupPresentOnlyRequiresObserverPolicyOnlyMode) {
    OverlayConfig cfg{};

    cfg.observerOnly = true;
    cfg.observerPolicyOnly = true;
    cfg.observerStartupPresentOnly = true;
    EXPECT_TRUE(IsOverlayObserverStartupPresentOnly(cfg));

    cfg.observerPolicyOnly = false;
    EXPECT_FALSE(IsOverlayObserverStartupPresentOnly(cfg));

    cfg.observerOnly = false;
    cfg.observerPolicyOnly = true;
    EXPECT_FALSE(IsOverlayObserverStartupPresentOnly(cfg));
}

TEST(StreamlineRuntimePolicyTest, PureObserverOnlyBehaviorRequiresPolicyProbeToStayDisabled) {
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldKeepPureObserverOnlyStreamlineBehavior(true, false));

    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldKeepPureObserverOnlyStreamlineBehavior(true, true));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldKeepPureObserverOnlyStreamlineBehavior(false, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldKeepPureObserverOnlyStreamlineBehavior(false, true));
}

TEST(StreamlineRuntimePolicyTest, ObserverPolicyOnlyPreservesStartupTransitionWindowWhilePostSLStaysDisabled) {
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldPreserveObserverPolicyOnlyStartupTransitionWindow(true, true));

    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldPreserveObserverPolicyOnlyStartupTransitionWindow(true, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldPreserveObserverPolicyOnlyStartupTransitionWindow(false, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldPreserveObserverPolicyOnlyStartupTransitionWindow(false, true));
}

TEST(StreamlineRuntimePolicyTest, StreamlineModuleUnloadDispatchesHookInvalidation) {
    // Crash 20260612_003407: the game unloads/reloads the whole SL stack when
    // toggling DLSS FG; every sl.* unload must invalidate stale hook slots.
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldInvalidateStreamlineHooksOnModuleUnload("sl.interposer.dll"));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldInvalidateStreamlineHooksOnModuleUnload("sl.common.dll"));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldInvalidateStreamlineHooksOnModuleUnload("sl.dlss_g.dll"));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldInvalidateStreamlineHooksOnModuleUnload("SL.COMMON.DLL"));

    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldInvalidateStreamlineHooksOnModuleUnload("slang.dll"));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldInvalidateStreamlineHooksOnModuleUnload("common.dll"));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldInvalidateStreamlineHooksOnModuleUnload("dxgi.dll"));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldInvalidateStreamlineHooksOnModuleUnload(nullptr));
}

TEST(StreamlineRuntimePolicyTest, HookSlotInvalidationMatchesUnloadedImageRange) {
    alignas(16) static unsigned char image[0x100];
    alignas(16) static unsigned char outsideImage[0x100];
    const void* base = image;
    const size_t size = sizeof(image);
    void* inRange = image + 0x40;
    void* pastEnd = image + sizeof(image);
    void* outside = outsideImage + 0x40;

    // Patched target inside the departing image.
    EXPECT_TRUE(
        ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(inRange, nullptr, base, size));
    // Saved original/export inside the departing image (import-fallback slots).
    EXPECT_TRUE(
        ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(nullptr, inRange, base, size));

    EXPECT_FALSE(
        ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(outside, pastEnd, base, size));
    EXPECT_FALSE(
        ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(nullptr, nullptr, base, size));
    EXPECT_FALSE(
        ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(inRange, inRange, nullptr, size));
    EXPECT_FALSE(
        ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(inRange, inRange, base, 0));
}

TEST(StreamlineRuntimePolicyTest, ReloadedCoreModuleMaskIsStaleWhenNoTargetBelongsToArrivingInstance) {
    EXPECT_TRUE(ce::streamline_runtime_policy::IsInstalledStreamlineModuleMaskStaleForReloadedModule(true, false));

    EXPECT_FALSE(ce::streamline_runtime_policy::IsInstalledStreamlineModuleMaskStaleForReloadedModule(true, true));
    EXPECT_FALSE(ce::streamline_runtime_policy::IsInstalledStreamlineModuleMaskStaleForReloadedModule(false, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::IsInstalledStreamlineModuleMaskStaleForReloadedModule(false, true));
}

// ---------------------------------------------------------------------------
// DLSSG activation-health monitor (session 20260702_094955): GTA cold-start DLSS FG reported ON
// (optionsMode=on, updateActive=1) but presents stayed at base rate all session — the health monitor must
// warn deterministically (sample streaks, not wall-clock) and only for ON-request samples so an OFF phase
// never extends or misattributes a streak.
// ---------------------------------------------------------------------------
TEST(StreamlineRuntimePolicyTest, DLSSGHealthTracksOnlySuccessfulOnRequestSamples) {
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldTrackDLSSGActivationHealthSample(true, true));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldTrackDLSSGActivationHealthSample(true, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldTrackDLSSGActivationHealthSample(false, true));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldTrackDLSSGActivationHealthSample(false, false));
}

TEST(StreamlineRuntimePolicyTest, DLSSGInterpolationEvidenceRequiresGeneratedFramePresented) {
    // ==1: only the real frame reached presentation (the failing GTA session's steady value).
    EXPECT_FALSE(ce::streamline_runtime_policy::IsDLSSGInterpolationPresentEvidence(0));
    EXPECT_FALSE(ce::streamline_runtime_policy::IsDLSSGInterpolationPresentEvidence(1));
    // >=2: real + generated frames presented (2x FG); MFG presents more.
    EXPECT_TRUE(ce::streamline_runtime_policy::IsDLSSGInterpolationPresentEvidence(2));
    EXPECT_TRUE(ce::streamline_runtime_policy::IsDLSSGInterpolationPresentEvidence(4));
}

TEST(StreamlineRuntimePolicyTest, DLSSGHealthWarnsAtStreakThenRepeatsSparsely) {
    using ce::streamline_runtime_policy::ShouldWarnDLSSGActiveButNotInterpolating;
    // Below the streak threshold: silent.
    EXPECT_FALSE(ShouldWarnDLSSGActiveButNotInterpolating(0, 8, 512));
    EXPECT_FALSE(ShouldWarnDLSSGActiveButNotInterpolating(7, 8, 512));
    // First warning exactly at the threshold.
    EXPECT_TRUE(ShouldWarnDLSSGActiveButNotInterpolating(8, 8, 512));
    // Then sparse repeats at the repeat cadence, silent in between.
    EXPECT_FALSE(ShouldWarnDLSSGActiveButNotInterpolating(9, 8, 512));
    EXPECT_FALSE(ShouldWarnDLSSGActiveButNotInterpolating(519, 8, 512));
    EXPECT_TRUE(ShouldWarnDLSSGActiveButNotInterpolating(520, 8, 512));
    EXPECT_TRUE(ShouldWarnDLSSGActiveButNotInterpolating(1032, 8, 512));
    // Degenerate configs never warn / never divide by zero.
    EXPECT_FALSE(ShouldWarnDLSSGActiveButNotInterpolating(100, 0, 512));
    EXPECT_FALSE(ShouldWarnDLSSGActiveButNotInterpolating(100, 8, 0));
    EXPECT_TRUE(ShouldWarnDLSSGActiveButNotInterpolating(8, 8, 0));
}

}  // namespace
