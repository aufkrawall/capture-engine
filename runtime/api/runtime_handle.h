// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

// The object behind a ce_runtime_t handle, and the guard every entry point runs its body under. Internal to
// runtime/api: the C ABI files share it, nothing outside the API module includes it.

#include "include/cengine/cengine.h"
#include "common/logging/logging.h"
#include "runtime/api/handle_registry.h"
#include "runtime/core/core.h"

#include <memory>
#include <new>
#include <string>
#include <utility>

// What ce_runtime_create hands out: the engine, and the client name its log lines carry.
struct ce_runtime {
    std::unique_ptr<ce::engine::Runtime> engine;
    std::string clientName;  // "unnamed" when the client passed no name
};

namespace ce::capi {

// Runs an entry point's body. A C++ exception never reaches the client: out-of-memory is CE_E_NO_MEMORY, anything
// else CE_E_INTERNAL, both logged with the entry point's name.
template <class Body>
ce_status_t RunGuarded(const char* entryPoint, Body&& body) noexcept {
    try {
        return body();
    } catch (const std::bad_alloc&) {
        LogWarn("[CApi] %s: out of memory", entryPoint);
        return CE_E_NO_MEMORY;
    } catch (...) {
        LogError("[CApi] %s: unexpected exception", entryPoint);
        return CE_E_INTERNAL;
    }
}

}  // namespace ce::capi
