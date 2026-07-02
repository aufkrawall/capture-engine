#include "inject_overlay_policy.h"

InjectorConfigState BuildInjectorConfigState(const AppConfig& config) {
    // Explicit screen-grab methods (wgc, dxgi_dup) never inject for capture;
    // they still allow overlay-only injection via overlay_whitelist.
    const bool wgcMode = IsScreenGrabCaptureMethod(config.captureMethod);
    const bool overlayOnlyInjection = wgcMode && !config.overlayWhitelist.empty();
    const bool hasInjectionTargets = !config.gameWhitelist.empty() || !config.overlayWhitelist.empty();

    InjectorConfigState state;
    state.config = config;
    state.allowInjection = hasInjectionTargets && (!wgcMode || overlayOnlyInjection);

    if (!state.allowInjection) {
        state.config.gameWhitelist.clear();
        state.config.overlayWhitelist.clear();
    } else if (overlayOnlyInjection) {
        state.config.gameWhitelist.clear();
    }

    return state;
}

bool ShouldRescanForConfigChange(const AppConfig& oldBaseConfig, const InjectorConfigState& oldState,
                                 const AppConfig& newBaseConfig, const InjectorConfigState& newState) {
    return oldState.allowInjection != newState.allowInjection || oldBaseConfig.logLevel != newBaseConfig.logLevel ||
           oldState.config.gameWhitelist != newState.config.gameWhitelist ||
           oldState.config.overlayWhitelist != newState.config.overlayWhitelist;
}

bool ShouldSuppressPseudoOverlayForInjectOverlayHandoff(bool injectOverlayPending, bool injectOverlayActive) {
    return injectOverlayPending || injectOverlayActive;
}
