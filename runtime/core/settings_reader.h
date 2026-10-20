// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

// The settings of a runtime as a client thread may read them (llm-wiki/library/03 "Settings"): the stored value of a
// key, the revision, and the validation of edits before they are admitted as a commit. Every member may be called
// from any thread; each takes only the store's own locks and never waits for the engine thread. Edits are written by
// the engine's commit command alone.

#include "runtime/settings/settings_store.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ce::engine {

class SettingsReader {
public:
    // A null store (a runtime assembled without one) reads as empty and refuses every check.
    explicit SettingsReader(std::shared_ptr<ce::runtime::SettingsStore> store);

    // The stored text of a key (UTF-8, not its default); nullopt when the key is absent.
    std::optional<std::string> Value(std::string_view section, std::string_view key) const;
    // The revision of the published settings; 0 without a store.
    uint64_t Revision() const;
    // What a commit of these edits against `baseRevision` would return now, without writing. IoError without a store.
    ce::runtime::CommitResult Check(const std::vector<ce::runtime::SettingsEdit>& edits, uint64_t baseRevision) const;

private:
    std::shared_ptr<ce::runtime::SettingsStore> store_;
};

}  // namespace ce::engine
