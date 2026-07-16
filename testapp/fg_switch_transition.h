#pragma once

namespace testapp::fg {

enum class FsrExitTransitionStage {
    None,
    PresentPending,
    PassthroughPresented,
    ReplacementPresentPending,
    ReplacementPresented,
};

enum class FsrExitTransitionAction {
    ContinueSwitch,
    DisableAndPresentPassthrough,
    PresentPassthrough,
};

// An FSR-owned proxy must present once after FG is disabled and remain alive until the replacement
// runtime is ready. Destroying the proxy after only the passthrough Present can remove the DWM
// presentation surface while a cold replacement runtime is still loading.
inline FsrExitTransitionAction ResolveFsrExitTransitionAction(bool currentModeIsFsr, bool targetModeIsFsr,
                                                              bool fsrLogicallyEnabled, FsrExitTransitionStage stage) {
    if (!currentModeIsFsr || targetModeIsFsr) {
        return FsrExitTransitionAction::ContinueSwitch;
    }
    if (stage == FsrExitTransitionStage::PresentPending) {
        return FsrExitTransitionAction::PresentPassthrough;
    }
    if (stage == FsrExitTransitionStage::None && fsrLogicallyEnabled) {
        return FsrExitTransitionAction::DisableAndPresentPassthrough;
    }
    return FsrExitTransitionAction::ContinueSwitch;
}

inline bool ShouldPrepareDlssBeforeFsrPresentationBreak(bool currentModeIsFsr, bool targetModeIsDlss,
                                                        bool dlssRuntimeReady, FsrExitTransitionStage stage) {
    return currentModeIsFsr && targetModeIsDlss && !dlssRuntimeReady &&
           stage == FsrExitTransitionStage::PassthroughPresented;
}

inline bool CanCommitFsrPresentationBreak(bool currentModeIsFsr, bool targetModeIsDlss, bool dlssRuntimeReady,
                                          FsrExitTransitionStage stage) {
    if (!currentModeIsFsr || stage != FsrExitTransitionStage::PassthroughPresented) {
        return false;
    }
    return !targetModeIsDlss || dlssRuntimeReady;
}

inline bool IsDlssReplacementSurfaceStage(FsrExitTransitionStage stage) {
    return stage == FsrExitTransitionStage::ReplacementPresentPending ||
           stage == FsrExitTransitionStage::ReplacementPresented;
}

// Creating the Streamline proxy is not enough to preserve the displayed surface: its first
// DLSS-G-active Present lazily initializes substantial feature resources. First make the new proxy
// visible with one ordinary FG-off Present, then allow DLSS-G activation on the following frame.
inline bool ShouldDeferDlssActivationUntilReplacementPresent(bool targetModeIsDlss, FsrExitTransitionStage stage) {
    return targetModeIsDlss && stage == FsrExitTransitionStage::ReplacementPresentPending;
}

}  // namespace testapp::fg
