#pragma once

#include <string>

#include "config.h"

struct InjectorConfigState {
    AppConfig config;
    bool allowInjection = false;
};

// The overlay-toggle hotkey is a runtime override, never a config edit. The
// inject process publishes exactly one resolved config for the active injected
// target, so expressing the toggle by rewriting the loaded config republishes
// that config — and republishing anything that was not resolved against the
// active target silently drops the target profile's graphics, DLSS and UE5
// overrides while the game keeps running.
struct OverlayVisibilityOverride {
    bool active = false;
    bool showOverlay = false;
};

InjectorConfigState BuildInjectorConfigState(const AppConfig& config);
bool ShouldRescanForConfigChange(const AppConfig& oldBaseConfig, const InjectorConfigState& oldState,
                                 const AppConfig& newBaseConfig, const InjectorConfigState& newState);
bool ShouldSuppressPseudoOverlayForInjectOverlayHandoff(bool injectOverlayPending, bool injectOverlayActive);

// Visibility a publication carries: the runtime override once the hotkey armed
// one, otherwise whatever the resolved config configures.
bool ResolveOverlayVisibility(const OverlayVisibilityOverride& runtimeOverride, bool configuredShowOverlay);

// Next override for one hotkey press. The press flips the effective visibility
// rather than the configured one, so a profile that overrides [Overlay] enabled
// cannot turn the first press into a no-op.
OverlayVisibilityOverride ToggleOverlayVisibility(const OverlayVisibilityOverride& runtimeOverride,
                                                  bool configuredShowOverlay);

// Process whose [Profile.*] section a publication resolves against. The target
// the injector last identified wins because it is the newer event; the live hook
// source covers the case where no injection has been observed in this session.
std::string ResolveActiveTargetProcessName(const std::string& lastPublishedTargetProcessName,
                                           const std::string& hookSourceProcessName);

// Config a publication must carry, before any runtime override. Loading with the
// target's process name is what pulls that target's [Profile.*] section in.
AppConfig ResolveTargetConfig(const std::string& configPath, const AppConfig& baseConfig,
                              const std::string& targetProcessName);

// Stamps the runtime overlay visibility onto an already resolved config. Kept
// separate from resolution so a caller can read the configured visibility first,
// which is what the toggle flips.
void ApplyOverlayVisibility(const OverlayVisibilityOverride& runtimeOverride, AppConfig& config);
