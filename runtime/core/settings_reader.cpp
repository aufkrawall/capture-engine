// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "runtime/core/settings_reader.h"

namespace ce::engine {

SettingsReader::SettingsReader(std::shared_ptr<ce::runtime::SettingsStore> store) : store_(std::move(store)) {}

std::optional<std::string> SettingsReader::Value(std::string_view section, std::string_view key) const {
    if (!store_)
        return std::nullopt;
    return store_->Value(section, key);
}

uint64_t SettingsReader::Revision() const {
    return store_ ? store_->Revision() : 0;
}

ce::runtime::CommitResult SettingsReader::Check(const std::vector<ce::runtime::SettingsEdit>& edits,
                                                uint64_t baseRevision) const {
    if (!store_) {
        ce::runtime::CommitResult result;
        result.status = ce::runtime::CommitStatus::IoError;
        return result;
    }
    return store_->Check(edits, baseRevision);
}

}  // namespace ce::engine
