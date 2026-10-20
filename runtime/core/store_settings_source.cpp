// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "runtime/core/store_settings_source.h"

namespace ce::engine {

StoreSettingsSource::StoreSettingsSource(std::shared_ptr<ce::runtime::SettingsStore> store)
    : store_(std::move(store)), current_(store_->Current()) {}

std::optional<AppConfig> StoreSettingsSource::Poll() {
    if (!store_->Poll())
        return std::nullopt;
    std::optional<AppConfig> previous(*current_);
    current_ = store_->Current();
    return previous;
}

uint32_t StoreSettingsSource::WaitMs() const {
    return store_->NextPollDelayMs();
}

uint64_t StoreSettingsSource::Revision() const {
    return store_->Revision();
}

std::optional<std::string> StoreSettingsSource::TakeRejection() {
    return store_->TakeRejection();
}

ce::runtime::CommitResult StoreSettingsSource::Commit(const std::vector<ce::runtime::SettingsEdit>& edits,
                                                      uint64_t baseRevision, std::optional<AppConfig>& previous) {
    const uint64_t before = store_->Revision();
    ce::runtime::CommitResult result = store_->Commit(edits, baseRevision);
    if (store_->Revision() != before) {
        previous = *current_;
        current_ = store_->Current();
    }
    return result;
}

std::unique_ptr<SettingsSource> MakeStoreSettings(std::shared_ptr<ce::runtime::SettingsStore> store) {
    return std::make_unique<StoreSettingsSource>(std::move(store));
}

}  // namespace ce::engine
