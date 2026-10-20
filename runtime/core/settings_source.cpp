// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "runtime/core/settings_source.h"

#include "runtime/settings/runtime_configuration.h"

namespace ce::engine {
namespace {
class RuntimeConfigurationSettings final : public SettingsSource {
public:
    const AppConfig& Current() const override { return ce::runtime::RuntimeConfiguration(); }
    std::optional<AppConfig> Poll() override { return ce::runtime::PollRuntimeConfiguration(); }
    uint32_t WaitMs() const override { return ce::runtime::RuntimeConfigurationWaitMs(); }
};
}  // namespace

std::unique_ptr<SettingsSource> MakeRuntimeConfigurationSettings() {
    return std::make_unique<RuntimeConfigurationSettings>();
}

}  // namespace ce::engine
