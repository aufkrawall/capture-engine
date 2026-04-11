#include <gtest/gtest.h>

#include "../hook/common/dx12_fg_transition_model.h"

namespace {

using ce::dx12_fg_transition::Input;
using ce::dx12_fg_transition::ObservedRuntimeMode;
using ce::dx12_fg_transition::OverlayRenderMode;
using ce::dx12_fg_transition::State;
using ce::dx12_fg_transition::TransitionPhase;

State Step(const State& state, const Input& input) {
    return ce::dx12_fg_transition::Reduce(state, input);
}

}  // namespace

TEST(DX12FGTransitionSequencesTest, OffToFSRToOffUsesRuntimeOwnedRecoveryThenSettles) {
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

TEST(DX12FGTransitionSequencesTest, OffToDLSSToOffUsesPostSLThenDisables) {
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

TEST(DX12FGTransitionSequencesTest, FSRToDLSSIsSwitchingAndMovesToPostSL) {
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

TEST(DX12FGTransitionSequencesTest, DLSSToFSRIsSwitchingAndLeavesPostSL) {
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

TEST(DX12FGTransitionSequencesTest, DLSSSuspensionUsesSuspendedFallbackWithoutClearingStreamlineContext) {
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

TEST(DX12FGTransitionSequencesTest, StartupBypassAndSuppressionOverrideNormalRouting) {
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
