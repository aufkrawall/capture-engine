// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

// Test seam of the C ABI: lets a unit-test image assemble the engine from fakes (runtime/core/runtime_internal.h)
// through the public entry points. Compiled only with CE_UNIT_TESTS; the production image has no seam. Only
// runtime/api includes this header.

#ifdef CE_UNIT_TESTS

#include "runtime/core/core.h"

#include <functional>
#include <memory>
#include <utility>

namespace ce::capi::testing {

using EngineFactory =
    std::function<std::unique_ptr<ce::engine::Runtime>(const ce::engine::RuntimeOptions&, ce_status_t*)>;

// The factory ce_runtime_create uses for the rest of the process; an empty function restores the production
// assembly. While one is set, package resolution is skipped: the options carry the descriptor's features and the
// calling module only. Set it from the test thread before creating a runtime.
inline EngineFactory& RuntimeFactorySlot() {
    static EngineFactory slot;
    return slot;
}

inline void SetRuntimeFactory(EngineFactory factory) {
    RuntimeFactorySlot() = std::move(factory);
}

}  // namespace ce::capi::testing

#endif  // CE_UNIT_TESTS
