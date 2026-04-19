#include <gtest/gtest.h>

#include "../hook/common/dx12_fg_transition_model.h"
#include "../hook/common/fg_detection.h"
#include "../hook/common/fg_session_state.h"

namespace {

using ce::dx12_fg_transition::Input;
using ce::dx12_fg_transition::ObservedRuntimeMode;
using ce::dx12_fg_transition::OverlayRenderMode;
using ce::dx12_fg_transition::State;
using ce::dx12_fg_transition::TransitionPhase;

ce::fg_session::DX12LegacyStateView g_TransitionLegacyState;

void FillTransitionLegacyState(ce::fg_session::DX12LegacyStateView* out) {
    if (!out) {
        return;
    }
    *out = g_TransitionLegacyState;
}

State Step(const State& state, const Input& input) {
    g_TransitionLegacyState = {};
    g_TransitionLegacyState.runtimeOwnsSwapchain = input.runtimeOwnsSwapchain;
    g_TransitionLegacyState.hadFSRPhase = input.hadFSRPhase;
    g_TransitionLegacyState.postSLCallbackInstalled =
        input.streamlineFGRunning && input.runtimeMode == ce::fg_runtime::RuntimeMode::kDLSSFG;
    g_TransitionLegacyState.postSLActive = g_TransitionLegacyState.postSLCallbackInstalled;
    g_TransitionLegacyState.postSLConfirmedRendering =
        g_TransitionLegacyState.postSLActive && input.effectiveFGActive && !input.startupBypassActive;
    g_TransitionLegacyState.postSLSettling = false;
    g_TransitionLegacyState.postSLStartupActivationPending =
        input.streamlineFGRunning && input.runtimeMode == ce::fg_runtime::RuntimeMode::kDLSSFG &&
        !g_TransitionLegacyState.postSLConfirmedRendering;
    g_TransitionLegacyState.postSLActiveButUnconfirmed =
        g_TransitionLegacyState.postSLActive && !g_TransitionLegacyState.postSLConfirmedRendering;
    g_TransitionLegacyState.observerOnly = input.overlaySuppressed;
    g_TransitionLegacyState.safePostFSRBootstrapPath = input.streamlineFGRunning || !input.hadFSRPhase;
    g_TransitionLegacyState.explicitSetOptionsActivationForCurrentComeback =
        input.streamlineFGRunning && input.runtimeMode == ce::fg_runtime::RuntimeMode::kDLSSFG && input.hadFSRPhase;

    g_FGCompat.SetStreamlineSupportPresent(input.streamlineLoaded);
    g_FGCompat.SetFSRFGSupportPresent(input.runtimeMode == ce::fg_runtime::RuntimeMode::kFSRFG);
    g_FGCompat.SetStreamlineFGSignal(input.streamlineFGRunning);
    g_FGCompat.SetDLSSFGActive(input.runtimeMode == ce::fg_runtime::RuntimeMode::kDLSSFG && input.effectiveFGActive);
    g_FGCompat.SetDLSSFGMultiplier(
        input.runtimeMode == ce::fg_runtime::RuntimeMode::kDLSSFG && input.effectiveFGActive ? 2 : 0);
    g_FGCompat.SetFSRFGActive(input.runtimeMode == ce::fg_runtime::RuntimeMode::kFSRFG && input.effectiveFGActive);
    g_FGCompat.SetFSRFGMultiplier(
        input.runtimeMode == ce::fg_runtime::RuntimeMode::kFSRFG && input.effectiveFGActive ? 2 : 0);
    g_FGCompat.SetHeuristicFSRFGActive(false);

    ce::fg_session::RegisterDX12LegacyStateProvider(&FillTransitionLegacyState);
    ce::fg_session::EmitFGEvent(input.streamlineFGRunning
                                    ? ce::fg_session::FGEventKind::kStreamlineSetOptionsRuntimeUpdate
                                    : ce::fg_session::FGEventKind::kPresentObserved,
                                "test-dx12-fg-transition");
    return ce::dx12_fg_transition::Reduce(state, input);
}

}  // namespace

class DX12FGTransitionSequencesFixture : public ::testing::Test {
protected:
    void SetUp() override {
        ce::fg_session::ResetFGSessionStateForTests();
        g_TransitionLegacyState = {};
        ce::fg_session::RegisterDX12LegacyStateProvider(&FillTransitionLegacyState);
        g_FGCompat.SetStreamlineSupportPresent(false);
        g_FGCompat.SetFSRFGSupportPresent(false);
        g_FGCompat.SetStreamlineFGSignal(false);
        g_FGCompat.SetDLSSFGActive(false);
        g_FGCompat.SetDLSSFGMultiplier(0);
        g_FGCompat.SetFSRFGActive(false);
        g_FGCompat.SetFSRFGMultiplier(0);
        g_FGCompat.SetHeuristicFSRFGActive(false);
        g_FGCompat.ClearNvidiaSMState();
    }
};

TEST_F(DX12FGTransitionSequencesFixture, OffToFSRToOffUsesRuntimeOwnedRecoveryThenSettles) {
    State state;

    state = Step(state, {.runtimeMode = ce::fg_runtime::RuntimeMode::kOff});
    EXPECT_EQ(state.snapshot.observedMode, ObservedRuntimeMode::kOff);
    EXPECT_EQ(state.snapshot.renderMode, OverlayRenderMode::kNormalPreSL);
    EXPECT_EQ(state.snapshot.phase, TransitionPhase::kStable);
    EXPECT_FALSE(state.snapshot.publishFGActive);

    state = Step(state, {.runtimeMode = ce::fg_runtime::RuntimeMode::kFSRFG,
                         .effectiveFGActive = true,
                         .runtimeOwnsSwapchain = true,
                         .hadFSRPhase = true});
    EXPECT_EQ(state.snapshot.observedMode, ObservedRuntimeMode::kFSRFG);
    EXPECT_EQ(state.snapshot.renderMode, OverlayRenderMode::kRuntimeOwnedPreSL);
    EXPECT_EQ(state.snapshot.phase, TransitionPhase::kEnabling);
    EXPECT_TRUE(state.snapshot.publishFGActive);

    state = Step(state, {.runtimeMode = ce::fg_runtime::RuntimeMode::kOff,
                         .effectiveFGActive = false,
                         .hadFSRPhase = true,
                         .recoveringPostFSRNonFG = true});
    EXPECT_EQ(state.snapshot.renderMode, OverlayRenderMode::kRecoveryPostFSROff);
    EXPECT_EQ(state.snapshot.phase, TransitionPhase::kRecovering);
    EXPECT_FALSE(state.snapshot.publishFGActive);
    EXPECT_TRUE(state.snapshot.shouldSuppressHeuristics);

    state = Step(state, {.runtimeMode = ce::fg_runtime::RuntimeMode::kOff, .effectiveFGActive = false});
    EXPECT_EQ(state.snapshot.renderMode, OverlayRenderMode::kNormalPreSL);
    EXPECT_EQ(state.snapshot.phase, TransitionPhase::kStable);
    EXPECT_FALSE(state.snapshot.shouldSuppressHeuristics);
}

TEST_F(DX12FGTransitionSequencesFixture, OffToDLSSToOffUsesPostSLThenDisables) {
    State state;

    state = Step(state, {.runtimeMode = ce::fg_runtime::RuntimeMode::kDLSSFG,
                         .effectiveFGActive = true,
                         .streamlineFGRunning = true,
                         .streamlineLoaded = true});
    EXPECT_EQ(state.snapshot.renderMode, OverlayRenderMode::kPostSL);
    EXPECT_EQ(state.snapshot.phase, TransitionPhase::kEnabling);
    EXPECT_TRUE(state.snapshot.shouldInstallPostSLCallback);
    EXPECT_EQ(state.snapshot.publishRuntimeMode, ce::fg_runtime::RuntimeMode::kDLSSFG);

    state =
        Step(state,
             {.runtimeMode = ce::fg_runtime::RuntimeMode::kOff, .effectiveFGActive = false, .streamlineLoaded = false});
    EXPECT_EQ(state.snapshot.renderMode, OverlayRenderMode::kNormalPreSL);
    EXPECT_EQ(state.snapshot.phase, TransitionPhase::kDisabling);
    EXPECT_FALSE(state.snapshot.shouldInstallPostSLCallback);
    EXPECT_FALSE(state.snapshot.publishFGActive);
}

TEST_F(DX12FGTransitionSequencesFixture, LateStreamlineRuntimeConvergenceToDLSSDoesNotReenterEnablingPhase) {
    State state;

    state = Step(state, {.runtimeMode = ce::fg_runtime::RuntimeMode::kStreamlineNoFG,
                         .effectiveFGActive = true,
                         .streamlineFGRunning = true,
                         .streamlineLoaded = true});
    EXPECT_EQ(state.snapshot.phase, TransitionPhase::kEnabling);
    EXPECT_EQ(state.snapshot.renderMode, OverlayRenderMode::kNormalPreSL);

    state = Step(state, {.runtimeMode = ce::fg_runtime::RuntimeMode::kDLSSFG,
                         .effectiveFGActive = true,
                         .streamlineFGRunning = true,
                         .streamlineLoaded = true});
    EXPECT_EQ(state.snapshot.phase, TransitionPhase::kStable);
    EXPECT_EQ(state.snapshot.renderMode, OverlayRenderMode::kPostSL);
    EXPECT_TRUE(state.snapshot.shouldInstallPostSLCallback);
    EXPECT_TRUE(state.snapshot.publishFGActive);
    EXPECT_EQ(state.snapshot.publishRuntimeMode, ce::fg_runtime::RuntimeMode::kDLSSFG);
}

TEST_F(DX12FGTransitionSequencesFixture, FSRToDLSSIsSwitchingAndMovesToPostSL) {
    State state;

    state = Step(state, {.runtimeMode = ce::fg_runtime::RuntimeMode::kFSRFG,
                         .effectiveFGActive = true,
                         .runtimeOwnsSwapchain = true,
                         .hadFSRPhase = true});
    state = Step(state, {.runtimeMode = ce::fg_runtime::RuntimeMode::kDLSSFG,
                         .effectiveFGActive = true,
                         .streamlineFGRunning = true,
                         .streamlineLoaded = true,
                         .hadFSRPhase = true});

    EXPECT_EQ(state.snapshot.phase, TransitionPhase::kSwitching);
    EXPECT_EQ(state.snapshot.renderMode, OverlayRenderMode::kPostSL);
    EXPECT_TRUE(state.snapshot.shouldInstallPostSLCallback);
    EXPECT_TRUE(state.snapshot.shouldSuppressHeuristics);
    EXPECT_EQ(state.snapshot.publishRuntimeMode, ce::fg_runtime::RuntimeMode::kDLSSFG);
}

TEST_F(DX12FGTransitionSequencesFixture, DLSSToFSRIsSwitchingAndLeavesPostSL) {
    State state;

    state = Step(state, {.runtimeMode = ce::fg_runtime::RuntimeMode::kDLSSFG,
                         .effectiveFGActive = true,
                         .streamlineFGRunning = true,
                         .streamlineLoaded = true});
    state = Step(state, {.runtimeMode = ce::fg_runtime::RuntimeMode::kFSRFG,
                         .effectiveFGActive = true,
                         .runtimeOwnsSwapchain = true,
                         .hadFSRPhase = true});

    EXPECT_EQ(state.snapshot.phase, TransitionPhase::kSwitching);
    EXPECT_EQ(state.snapshot.renderMode, OverlayRenderMode::kRuntimeOwnedPreSL);
    EXPECT_FALSE(state.snapshot.shouldInstallPostSLCallback);
    EXPECT_EQ(state.snapshot.publishRuntimeMode, ce::fg_runtime::RuntimeMode::kFSRFG);
}

TEST_F(DX12FGTransitionSequencesFixture, DLSSSuspensionUsesSuspendedFallbackWithoutClearingStreamlineContext) {
    State state;

    state = Step(state, {.runtimeMode = ce::fg_runtime::RuntimeMode::kDLSSFG,
                         .effectiveFGActive = true,
                         .streamlineFGRunning = true,
                         .streamlineLoaded = true});
    state = Step(state, {.runtimeMode = ce::fg_runtime::RuntimeMode::kDLSSFG,
                         .effectiveFGActive = true,
                         .streamlineFGRunning = false,
                         .streamlineLoaded = true});

    EXPECT_EQ(state.snapshot.phase, TransitionPhase::kSuspended);
    EXPECT_EQ(state.snapshot.renderMode, OverlayRenderMode::kSuspendedFallback);
    EXPECT_TRUE(state.snapshot.shouldInstallPostSLCallback);
    EXPECT_TRUE(state.snapshot.publishFGActive);
}

TEST_F(DX12FGTransitionSequencesFixture, StartupBypassAndSuppressionOverrideNormalRouting) {
    State state;

    state = Step(
        state,
        {.runtimeMode = ce::fg_runtime::RuntimeMode::kOff, .startupBypassActive = true, .overlaySuppressed = false});
    EXPECT_EQ(state.snapshot.renderMode, OverlayRenderMode::kStartupBypass);
    EXPECT_TRUE(state.snapshot.overlayAllowed);

    state = Step(
        state,
        {.runtimeMode = ce::fg_runtime::RuntimeMode::kOff, .startupBypassActive = true, .overlaySuppressed = true});
    EXPECT_EQ(state.snapshot.renderMode, OverlayRenderMode::kSuppressed);
    EXPECT_FALSE(state.snapshot.overlayAllowed);
}
