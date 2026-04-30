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

    EXPECT_TRUE(getStateWarmupProtection);
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedStreamlineComeback(
        false, false, true, false, false, false, true, false, getStateWarmupProtection));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldDropSuppressedOffChurnForStartupProtectedStreamlineComeback(
        false, true, false, true, false, getStateWarmupProtection));
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

TEST(StreamlineRuntimePolicyTest, DirectPostSLCallbackTriggerStopsAfterActivationCompletes) {
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

}  // namespace
