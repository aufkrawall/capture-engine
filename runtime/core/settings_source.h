// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include "common/config/config.h"

#include <cstdint>
#include <optional>
#include <string>

namespace ce::engine {

// The published settings as the runtime sees them: one immutable snapshot, replaced as a whole. Production
// reads the transactional store (store_settings_source.h); the core tests use fakes. Engine-thread only,
// except where a method says otherwise.
class SettingsSource {
public:
    virtual ~SettingsSource() = default;
    virtual const AppConfig& Current() const = 0;
    // The previous snapshot when a complete replacement was published since the last call.
    virtual std::optional<AppConfig> Poll() = 0;
    // Milliseconds until a settings check is due (0 = now).
    virtual uint32_t WaitMs() const = 0;
    // The revision of the published settings; 0 for a source without one. Any thread.
    virtual uint64_t Revision() const { return 0; }
    // The refusal of an external edit since the last call, reported once: the engine announces it as
    // SETTINGS_REJECTED. Sources that never refuse return nullopt.
    virtual std::optional<std::string> TakeRejection() { return std::nullopt; }
};

}  // namespace ce::engine
