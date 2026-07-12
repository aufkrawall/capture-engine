#pragma once

#include "fg_runtime_state.h"

namespace ce::dx12_fg_transition {

enum class ObservedRuntimeMode {
    kOff,
    kStreamlineNoFG,
    kDLSSFG,
    kFSRFG,
    kNvidiaSM,
    kUnknown,
};

enum class OverlayRenderMode {
    kNormalPreSL,
    kPostSL,
    kRuntimeOwnedPreSL,
    kRecoveryPostFSROff,
    kSuspendedFallback,
    kStartupBypass,
    kSuppressed,
};

enum class SwapchainOwnershipMode {
    kGameOwned,
    kRuntimeOwned,
    kUnknown,
};

enum class TransitionPhase {
    kStable,
    kEnabling,
    kDisabling,
    kSwitching,
    kSuspended,
    kRecovering,
};

struct Snapshot {
    ObservedRuntimeMode observedMode = ObservedRuntimeMode::kOff;
    OverlayRenderMode renderMode = OverlayRenderMode::kNormalPreSL;
    SwapchainOwnershipMode ownership = SwapchainOwnershipMode::kGameOwned;
    TransitionPhase phase = TransitionPhase::kStable;
    bool overlayAllowed = true;
    bool shouldInstallPostSLCallback = false;
    bool shouldSuppressHeuristics = false;
    bool publishFGActive = false;
    ce::fg_runtime::RuntimeMode publishRuntimeMode = ce::fg_runtime::RuntimeMode::kOff;
    uint32_t epoch = 0;
};

struct Input {
    ce::fg_runtime::RuntimeMode runtimeMode = ce::fg_runtime::RuntimeMode::kOff;
    bool effectiveFGActive = false;
    bool streamlineFGRunning = false;
    bool streamlineLoaded = false;
    bool runtimeOwnsSwapchain = false;
    bool hadFSRPhase = false;
    bool nativeFSRSuspended = false;
    bool recoveringPostFSRNonFG = false;
    bool startupBypassActive = false;
    bool overlaySuppressed = false;
};

struct State {
    Snapshot snapshot;
    bool previousFGActive = false;
    ce::fg_runtime::RuntimeMode previousRuntimeMode = ce::fg_runtime::RuntimeMode::kOff;
    bool previousStreamlineFGRunning = false;
};

ObservedRuntimeMode ToObservedRuntimeMode(ce::fg_runtime::RuntimeMode mode);
State Reduce(const State& state, const Input& input);

}  // namespace ce::dx12_fg_transition
