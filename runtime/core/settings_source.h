// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include "common/config/config.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace ce::engine {

// The published settings as the runtime sees them: one immutable snapshot, replaced as a whole.
// Production reads the process's runtime configuration owner (runtime/settings); the core tests use a
// fake. Engine-thread only.
class SettingsSource {
public:
    virtual ~SettingsSource() = default;
    virtual const AppConfig& Current() const = 0;
    // The previous snapshot when a complete replacement was published since the last call.
    virtual std::optional<AppConfig> Poll() = 0;
    // Milliseconds until a settings check is due (0 = now).
    virtual uint32_t WaitMs() const = 0;
};

// Adapter over ce::runtime::RuntimeConfiguration()/PollRuntimeConfiguration(). It is the one place in
// core that touches that global accessor; M5's SettingsStore replaces it.
std::unique_ptr<SettingsSource> MakeRuntimeConfigurationSettings();

}  // namespace ce::engine
