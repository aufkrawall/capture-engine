#pragma once

#include "common/config/config.h"

#include <memory>
#include <optional>
#include <string>

namespace ce::runtime {
// One host-thread settings owner using a resolved INI path. Construction preserves INI/default
// loading; it does not create a tray, install crash handling or initialize an engine.
class RuntimeConfigurationSession {
public:
    explicit RuntimeConfigurationSession(std::string path, uint32_t (*nowMs)() noexcept = nullptr);
    ~RuntimeConfigurationSession();
    RuntimeConfigurationSession(const RuntimeConfigurationSession&) = delete;
    RuntimeConfigurationSession& operator=(const RuntimeConfigurationSession&) = delete;
    bool IsReady() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Private application view, valid on the owning thread until its scope ends.
// Requires a ready session; no mutable settings reference escapes.
const AppConfig& RuntimeConfiguration();
void SetRuntimeProcessLogPath(std::string path);
std::optional<AppConfig> PollRuntimeConfiguration();
uint32_t RuntimeConfigurationWaitMs();
}  // namespace ce::runtime
