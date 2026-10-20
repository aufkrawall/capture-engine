// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

// The engine's settings source over the transactional store (llm-wiki/library/04 "runtime/settings"). The engine
// thread reads the snapshot the store last published; external edits arrive through Poll and refusals through
// TakeRejection. The source keeps its snapshot alive, so references from Current() stay valid until the next Poll
// or Commit on the engine thread.

#include "runtime/core/settings_source.h"
#include "runtime/settings/settings_store.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace ce::engine {

class StoreSettingsSource final : public SettingsSource {
public:
    explicit StoreSettingsSource(std::shared_ptr<ce::runtime::SettingsStore> store);
    const AppConfig& Current() const override { return *current_; }
    std::optional<AppConfig> Poll() override;
    uint32_t WaitMs() const override;
    uint64_t Revision() const override;
    std::optional<std::string> TakeRejection() override;

private:
    std::shared_ptr<ce::runtime::SettingsStore> store_;
    std::shared_ptr<const AppConfig> current_;  // the snapshot the engine thread is running on
};

std::unique_ptr<SettingsSource> MakeStoreSettings(std::shared_ptr<ce::runtime::SettingsStore> store);

}  // namespace ce::engine
