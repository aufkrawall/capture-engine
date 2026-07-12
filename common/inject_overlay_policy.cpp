#include "inject_overlay_policy.h"

InjectorConfigState BuildInjectorConfigState(const AppConfig& config) {
    // Injection is an independent capability. capture_method selects the video
    // acquisition backend; it must not suppress the injected overlay or graphics
    // overrides requested by the normal whitelist.
    const bool hasInjectionTargets = !config.gameWhitelist.empty() || !config.overlayWhitelist.empty();

    InjectorConfigState state;
    state.config = config;
    state.allowInjection = hasInjectionTargets;

    if (!state.allowInjection) {
        state.config.gameWhitelist.clear();
        state.config.overlayWhitelist.clear();
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
