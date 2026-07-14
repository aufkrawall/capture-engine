#include <gtest/gtest.h>

#include <vector>

#include "../hook/common/dx12_fg_transition_model.h"
#include "../hook/common/fg_detection.h"
#include "../hook/common/fg_session_state.h"

namespace {

using ce::dx12_fg_transition::Input;
using ce::dx12_fg_transition::ObservedRuntimeMode;
using ce::dx12_fg_transition::OverlayRenderMode;
using ce::dx12_fg_transition::State;
using ce::dx12_fg_transition::TransitionPhase;

ce::fg_session::DX12LegacyStateView g_ReplayLegacyState;

void FillReplayLegacyState(ce::fg_session::DX12LegacyStateView* out) {
    if (!out) {
        return;
    }
    *out = g_ReplayLegacyState;
}

struct ExpectedSnapshot {
    ObservedRuntimeMode observedMode;
    OverlayRenderMode renderMode;
    TransitionPhase phase;
    bool callbackInstalled;
    bool publishFGActive;
    ce::fg_runtime::RuntimeMode publishRuntimeMode;
};

struct TraceStep {
    const char* label;
    Input input;
    ExpectedSnapshot expected;
};

void ReplayTrace(const std::vector<TraceStep>& trace) {
    State state;
    for (const auto& step : trace) {
        g_ReplayLegacyState = {};
        g_ReplayLegacyState.runtimeOwnsSwapchain = step.input.runtimeOwnsSwapchain;
        g_ReplayLegacyState.hadFSRPhase = step.input.hadFSRPhase;
        g_ReplayLegacyState.postSLCallbackInstalled =
            step.input.streamlineFGRunning && step.input.runtimeMode == ce::fg_runtime::RuntimeMode::kDLSSFG;
        g_ReplayLegacyState.postSLActive = g_ReplayLegacyState.postSLCallbackInstalled;
        g_ReplayLegacyState.postSLConfirmedRendering =
            g_ReplayLegacyState.postSLActive && step.input.effectiveFGActive && !step.input.startupBypassActive;
        g_ReplayLegacyState.postSLStartupActivationPending =
            g_ReplayLegacyState.postSLActive && !g_ReplayLegacyState.postSLConfirmedRendering;
        g_ReplayLegacyState.postSLActiveButUnconfirmed = g_ReplayLegacyState.postSLStartupActivationPending;
        g_ReplayLegacyState.safePostFSRBootstrapPath = step.input.streamlineFGRunning || !step.input.hadFSRPhase;
        g_ReplayLegacyState.explicitSetOptionsActivationForCurrentComeback =
            step.input.streamlineFGRunning && step.input.runtimeMode == ce::fg_runtime::RuntimeMode::kDLSSFG &&
            step.input.hadFSRPhase;
        g_ReplayLegacyState.observerOnly = step.input.overlaySuppressed;

        g_FGCompat.SetStreamlineSupportPresent(step.input.streamlineLoaded);
        g_FGCompat.SetFSRFGSupportPresent(step.input.runtimeMode == ce::fg_runtime::RuntimeMode::kFSRFG);
        g_FGCompat.SetStreamlineFGSignal(step.input.streamlineFGRunning);
        g_FGCompat.SetDLSSFGActive(step.input.runtimeMode == ce::fg_runtime::RuntimeMode::kDLSSFG &&
                                   step.input.effectiveFGActive);
        g_FGCompat.SetDLSSFGMultiplier(
            step.input.runtimeMode == ce::fg_runtime::RuntimeMode::kDLSSFG && step.input.effectiveFGActive ? 2 : 0);
        g_FGCompat.SetFSRFGActive(step.input.runtimeMode == ce::fg_runtime::RuntimeMode::kFSRFG &&
                                  step.input.effectiveFGActive);
        g_FGCompat.SetFSRFGMultiplier(
            step.input.runtimeMode == ce::fg_runtime::RuntimeMode::kFSRFG && step.input.effectiveFGActive ? 2 : 0);
        g_FGCompat.SetHeuristicFSRFGActive(false);

        ce::fg_session::RegisterDX12LegacyStateProvider(&FillReplayLegacyState);
        ce::fg_session::EmitFGEvent(step.input.streamlineFGRunning
                                        ? ce::fg_session::FGEventKind::kStreamlineSetOptionsRuntimeUpdate
                                        : ce::fg_session::FGEventKind::kPresentObserved,
                                    step.label);
        state = ce::dx12_fg_transition::Reduce(state, step.input);
        EXPECT_EQ(state.snapshot.observedMode, step.expected.observedMode) << step.label;
        EXPECT_EQ(state.snapshot.renderMode, step.expected.renderMode) << step.label;
        EXPECT_EQ(state.snapshot.phase, step.expected.phase) << step.label;
        EXPECT_TRUE(state.snapshot.overlayAllowed) << step.label;
        EXPECT_EQ(state.snapshot.shouldInstallPostSLCallback, step.expected.callbackInstalled) << step.label;
        EXPECT_EQ(state.snapshot.publishFGActive, step.expected.publishFGActive) << step.label;
        EXPECT_EQ(state.snapshot.publishRuntimeMode, step.expected.publishRuntimeMode) << step.label;
    }
}

}  // namespace

class DX12FGTraceReplayFixture : public ::testing::Test {
protected:
    void SetUp() override {
        ce::fg_session::ResetFGSessionStateForTests();
        g_ReplayLegacyState = {};
        ce::fg_session::RegisterDX12LegacyStateProvider(&FillReplayLegacyState);
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

TEST_F(DX12FGTraceReplayFixture, TalosNoFGToDLSSOffToFSRReplay) {
    ReplayTrace({
        {"talos-off",
         {.runtimeMode = ce::fg_runtime::RuntimeMode::kOff},
         {ObservedRuntimeMode::kOff, OverlayRenderMode::kNormalPreSL, TransitionPhase::kStable, false, false,
          ce::fg_runtime::RuntimeMode::kOff}},
        {"talos-dlss-on",
         {.runtimeMode = ce::fg_runtime::RuntimeMode::kDLSSFG,
          .effectiveFGActive = true,
          .streamlineFGRunning = true,
          .streamlineLoaded = true},
         {ObservedRuntimeMode::kDLSSFG, OverlayRenderMode::kPostSL, TransitionPhase::kEnabling, true, true,
          ce::fg_runtime::RuntimeMode::kDLSSFG}},
        {"talos-dlss-off",
         {.runtimeMode = ce::fg_runtime::RuntimeMode::kOff, .effectiveFGActive = false},
         {ObservedRuntimeMode::kOff, OverlayRenderMode::kNormalPreSL, TransitionPhase::kDisabling, false, false,
          ce::fg_runtime::RuntimeMode::kOff}},
        {"talos-fsr-on",
         {.runtimeMode = ce::fg_runtime::RuntimeMode::kFSRFG,
          .effectiveFGActive = true,
          .runtimeOwnsSwapchain = true,
          .hadFSRPhase = true},
         {ObservedRuntimeMode::kFSRFG, OverlayRenderMode::kRuntimeOwnedPreSL, TransitionPhase::kEnabling, false, true,
          ce::fg_runtime::RuntimeMode::kFSRFG}},
    });
}

TEST_F(DX12FGTraceReplayFixture, GTAFSRToDLSSWithoutCleanOffReplay) {
    ReplayTrace({
        {"gta-fsr-on",
         {.runtimeMode = ce::fg_runtime::RuntimeMode::kFSRFG,
          .effectiveFGActive = true,
          .runtimeOwnsSwapchain = true,
          .hadFSRPhase = true},
         {ObservedRuntimeMode::kFSRFG, OverlayRenderMode::kRuntimeOwnedPreSL, TransitionPhase::kEnabling, false, true,
          ce::fg_runtime::RuntimeMode::kFSRFG}},
        {"gta-switch-to-dlss",
         {.runtimeMode = ce::fg_runtime::RuntimeMode::kDLSSFG,
          .effectiveFGActive = true,
          .streamlineFGRunning = true,
          .streamlineLoaded = true,
          .hadFSRPhase = true},
         {ObservedRuntimeMode::kDLSSFG, OverlayRenderMode::kPostSL, TransitionPhase::kSwitching, true, true,
          ce::fg_runtime::RuntimeMode::kDLSSFG}},
        {"gta-dlss-off",
         {.runtimeMode = ce::fg_runtime::RuntimeMode::kOff, .effectiveFGActive = false},
         {ObservedRuntimeMode::kOff, OverlayRenderMode::kNormalPreSL, TransitionPhase::kDisabling, false, false,
          ce::fg_runtime::RuntimeMode::kOff}},
    });
}

TEST_F(DX12FGTraceReplayFixture, AllOffFSRAndDLSSDirectionsKeepOverlayEligibleAndPublishExactStatus) {
    ReplayTrace({
        {"all-modes-off-start",
         {.runtimeMode = ce::fg_runtime::RuntimeMode::kOff},
         {ObservedRuntimeMode::kOff, OverlayRenderMode::kNormalPreSL, TransitionPhase::kStable, false, false,
          ce::fg_runtime::RuntimeMode::kOff}},
        {"all-modes-off-to-dlss",
         {.runtimeMode = ce::fg_runtime::RuntimeMode::kDLSSFG,
          .effectiveFGActive = true,
          .streamlineFGRunning = true,
          .streamlineLoaded = true},
         {ObservedRuntimeMode::kDLSSFG, OverlayRenderMode::kPostSL, TransitionPhase::kEnabling, true, true,
          ce::fg_runtime::RuntimeMode::kDLSSFG}},
        {"all-modes-dlss-to-off",
         {.runtimeMode = ce::fg_runtime::RuntimeMode::kOff},
         {ObservedRuntimeMode::kOff, OverlayRenderMode::kNormalPreSL, TransitionPhase::kDisabling, false, false,
          ce::fg_runtime::RuntimeMode::kOff}},
        {"all-modes-off-to-fsr",
         {.runtimeMode = ce::fg_runtime::RuntimeMode::kFSRFG,
          .effectiveFGActive = true,
          .runtimeOwnsSwapchain = true,
          .hadFSRPhase = true},
         {ObservedRuntimeMode::kFSRFG, OverlayRenderMode::kRuntimeOwnedPreSL, TransitionPhase::kEnabling, false, true,
          ce::fg_runtime::RuntimeMode::kFSRFG}},
        {"all-modes-fsr-to-dlss",
         {.runtimeMode = ce::fg_runtime::RuntimeMode::kDLSSFG,
          .effectiveFGActive = true,
          .streamlineFGRunning = true,
          .streamlineLoaded = true,
          .hadFSRPhase = true},
         {ObservedRuntimeMode::kDLSSFG, OverlayRenderMode::kPostSL, TransitionPhase::kSwitching, true, true,
          ce::fg_runtime::RuntimeMode::kDLSSFG}},
        {"all-modes-dlss-to-fsr",
         {.runtimeMode = ce::fg_runtime::RuntimeMode::kFSRFG,
          .effectiveFGActive = true,
          .runtimeOwnsSwapchain = true,
          .hadFSRPhase = true},
         {ObservedRuntimeMode::kFSRFG, OverlayRenderMode::kRuntimeOwnedPreSL, TransitionPhase::kSwitching, false, true,
          ce::fg_runtime::RuntimeMode::kFSRFG}},
        {"all-modes-fsr-to-off-recovery",
         {.runtimeMode = ce::fg_runtime::RuntimeMode::kOff,
          .hadFSRPhase = true,
          .recoveringPostFSRNonFG = true},
         {ObservedRuntimeMode::kOff, OverlayRenderMode::kRecoveryPostFSROff, TransitionPhase::kRecovering, false, false,
          ce::fg_runtime::RuntimeMode::kOff}},
        {"all-modes-off-to-fsr-reenable",
         {.runtimeMode = ce::fg_runtime::RuntimeMode::kFSRFG,
          .effectiveFGActive = true,
          .runtimeOwnsSwapchain = true,
          .hadFSRPhase = true},
         {ObservedRuntimeMode::kFSRFG, OverlayRenderMode::kRuntimeOwnedPreSL, TransitionPhase::kEnabling, false, true,
          ce::fg_runtime::RuntimeMode::kFSRFG}},
        {"all-modes-second-fsr-to-off-recovery",
         {.runtimeMode = ce::fg_runtime::RuntimeMode::kOff,
          .hadFSRPhase = true,
          .recoveringPostFSRNonFG = true},
         {ObservedRuntimeMode::kOff, OverlayRenderMode::kRecoveryPostFSROff, TransitionPhase::kRecovering, false, false,
          ce::fg_runtime::RuntimeMode::kOff}},
        {"all-modes-clean-off-return",
         {.runtimeMode = ce::fg_runtime::RuntimeMode::kOff, .hadFSRPhase = true},
         {ObservedRuntimeMode::kOff, OverlayRenderMode::kNormalPreSL, TransitionPhase::kStable, false, false,
          ce::fg_runtime::RuntimeMode::kOff}},
        {"all-modes-clean-off-to-dlss",
         {.runtimeMode = ce::fg_runtime::RuntimeMode::kDLSSFG,
          .effectiveFGActive = true,
          .streamlineFGRunning = true,
          .streamlineLoaded = true,
          .hadFSRPhase = true},
         {ObservedRuntimeMode::kDLSSFG, OverlayRenderMode::kPostSL, TransitionPhase::kEnabling, true, true,
          ce::fg_runtime::RuntimeMode::kDLSSFG}},
        {"all-modes-dlss-suspend",
         {.runtimeMode = ce::fg_runtime::RuntimeMode::kDLSSFG,
          .effectiveFGActive = true,
          .streamlineLoaded = true,
          .hadFSRPhase = true},
         {ObservedRuntimeMode::kDLSSFG, OverlayRenderMode::kSuspendedFallback, TransitionPhase::kSuspended, true, true,
          ce::fg_runtime::RuntimeMode::kDLSSFG}},
        {"all-modes-dlss-resume",
         {.runtimeMode = ce::fg_runtime::RuntimeMode::kDLSSFG,
          .effectiveFGActive = true,
          .streamlineFGRunning = true,
          .streamlineLoaded = true,
          .hadFSRPhase = true},
         {ObservedRuntimeMode::kDLSSFG, OverlayRenderMode::kPostSL, TransitionPhase::kStable, true, true,
          ce::fg_runtime::RuntimeMode::kDLSSFG}},
        {"all-modes-final-off",
         {.runtimeMode = ce::fg_runtime::RuntimeMode::kOff, .hadFSRPhase = true},
         {ObservedRuntimeMode::kOff, OverlayRenderMode::kNormalPreSL, TransitionPhase::kDisabling, false, false,
          ce::fg_runtime::RuntimeMode::kOff}},
    });
}

TEST_F(DX12FGTraceReplayFixture, GTADLSSSuspensionDuringLoadingReplay) {
    ReplayTrace({
        {"gta-dlss-on",
         {.runtimeMode = ce::fg_runtime::RuntimeMode::kDLSSFG,
          .effectiveFGActive = true,
          .streamlineFGRunning = true,
          .streamlineLoaded = true},
         {ObservedRuntimeMode::kDLSSFG, OverlayRenderMode::kPostSL, TransitionPhase::kEnabling, true, true,
          ce::fg_runtime::RuntimeMode::kDLSSFG}},
        {"gta-dlss-suspended",
         {.runtimeMode = ce::fg_runtime::RuntimeMode::kDLSSFG,
          .effectiveFGActive = true,
          .streamlineFGRunning = false,
          .streamlineLoaded = true},
         {ObservedRuntimeMode::kDLSSFG, OverlayRenderMode::kSuspendedFallback, TransitionPhase::kSuspended, true, true,
          ce::fg_runtime::RuntimeMode::kDLSSFG}},
        {"gta-dlss-resumed",
         {.runtimeMode = ce::fg_runtime::RuntimeMode::kDLSSFG,
          .effectiveFGActive = true,
          .streamlineFGRunning = true,
          .streamlineLoaded = true},
         {ObservedRuntimeMode::kDLSSFG, OverlayRenderMode::kPostSL, TransitionPhase::kStable, true, true,
          ce::fg_runtime::RuntimeMode::kDLSSFG}},
    });
}

TEST_F(DX12FGTraceReplayFixture, StartupCoexistenceReplayUsesStartupBypassThenNormalRouting) {
    ReplayTrace({
        {"startup-bypass",
         {.runtimeMode = ce::fg_runtime::RuntimeMode::kOff, .startupBypassActive = true},
         {ObservedRuntimeMode::kOff, OverlayRenderMode::kStartupBypass, TransitionPhase::kStable, false, false,
          ce::fg_runtime::RuntimeMode::kOff}},
        {"startup-settled",
         {.runtimeMode = ce::fg_runtime::RuntimeMode::kOff},
         {ObservedRuntimeMode::kOff, OverlayRenderMode::kNormalPreSL, TransitionPhase::kStable, false, false,
          ce::fg_runtime::RuntimeMode::kOff}},
    });
}
