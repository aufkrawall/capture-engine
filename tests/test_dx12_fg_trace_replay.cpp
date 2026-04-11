#include <gtest/gtest.h>

#include <vector>

#include "../hook/common/dx12_fg_transition_model.h"

namespace {

using ce::dx12_fg_transition::Input;
using ce::dx12_fg_transition::ObservedRuntimeMode;
using ce::dx12_fg_transition::OverlayRenderMode;
using ce::dx12_fg_transition::State;
using ce::dx12_fg_transition::TransitionPhase;

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
        state = ce::dx12_fg_transition::Reduce(state, step.input);
        EXPECT_EQ(state.snapshot.observedMode, step.expected.observedMode) << step.label;
        EXPECT_EQ(state.snapshot.renderMode, step.expected.renderMode) << step.label;
        EXPECT_EQ(state.snapshot.phase, step.expected.phase) << step.label;
        EXPECT_EQ(state.snapshot.shouldInstallPostSLCallback, step.expected.callbackInstalled) << step.label;
        EXPECT_EQ(state.snapshot.publishFGActive, step.expected.publishFGActive) << step.label;
        EXPECT_EQ(state.snapshot.publishRuntimeMode, step.expected.publishRuntimeMode) << step.label;
    }
}

}  // namespace

TEST(DX12FGTraceReplayTest, TalosNoFGToDLSSOffToFSRReplay) {
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

TEST(DX12FGTraceReplayTest, GTAFSRToDLSSWithoutCleanOffReplay) {
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

TEST(DX12FGTraceReplayTest, GTADLSSSuspensionDuringLoadingReplay) {
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

TEST(DX12FGTraceReplayTest, StartupCoexistenceReplayUsesStartupBypassThenNormalRouting) {
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
