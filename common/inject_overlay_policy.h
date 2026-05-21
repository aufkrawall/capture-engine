#pragma once

#include "config.h"

struct InjectorConfigState {
    AppConfig config;
    bool allowInjection = false;
};

InjectorConfigState BuildInjectorConfigState(const AppConfig& config);
bool ShouldRescanForConfigChange(const AppConfig& oldBaseConfig, const InjectorConfigState& oldState,
                                 const AppConfig& newBaseConfig, const InjectorConfigState& newState);
bool ShouldSuppressPseudoOverlayForInjectOverlayHandoff(bool injectOverlayPending, bool injectOverlayActive,
                                                       bool externalFallbackActive = false);
