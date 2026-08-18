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

bool ResolveOverlayVisibility(const OverlayVisibilityOverride& runtimeOverride, bool configuredShowOverlay) {
    return runtimeOverride.active ? runtimeOverride.showOverlay : configuredShowOverlay;
}

OverlayVisibilityOverride ToggleOverlayVisibility(const OverlayVisibilityOverride& runtimeOverride,
                                                  bool configuredShowOverlay) {
    OverlayVisibilityOverride toggled;
    toggled.active = true;
    toggled.showOverlay = !ResolveOverlayVisibility(runtimeOverride, configuredShowOverlay);
    return toggled;
}

std::string ResolveActiveTargetProcessName(const std::string& lastPublishedTargetProcessName,
                                           const std::string& hookSourceProcessName) {
    return lastPublishedTargetProcessName.empty() ? hookSourceProcessName : lastPublishedTargetProcessName;
}

AppConfig ResolveTargetConfig(const std::string& configPath, const AppConfig& baseConfig,
                              const std::string& targetProcessName) {
    AppConfig resolved = baseConfig;
    // An empty target name makes LoadConfig fall back to this process' own image
    // name, which would resolve a profile that has nothing to do with any target.
    if (!targetProcessName.empty()) {
        LoadConfig(configPath, resolved, targetProcessName);
    }
    return resolved;
}

void ApplyOverlayVisibility(const OverlayVisibilityOverride& runtimeOverride, AppConfig& config) {
    config.overlay.showOverlay = ResolveOverlayVisibility(runtimeOverride, config.overlay.showOverlay);
}
