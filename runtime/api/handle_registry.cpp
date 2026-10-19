// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "runtime/api/handle_registry.h"

#include "common/logging/log_meter.h"
#include "common/logging/logging.h"

namespace ce::capi {

void LogUnknownHandle(const char* entryPoint, uint64_t failedLookups) {
    if (ce::log_meter::ShouldLogCadence(static_cast<uint32_t>(failedLookups), 3, 64)) {
        LogWarn("[CApi] %s: handle is not a live runtime (NULL, destroyed or foreign) misses=%llu", entryPoint,
                static_cast<unsigned long long>(failedLookups));
    }
}

}  // namespace ce::capi
