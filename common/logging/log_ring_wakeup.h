// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include <windows.h>

#include "common/ipc/shared_defs_detail/session_namespace.h"

namespace ce::logging {

// One consumer drains every ring in this Windows session. Auto-reset coalesces a burst but retains a signal
// published before the consumer waits. Keep one handle per module until process exit, as with the hook's direct
// sink: logging entry points remain callable during static teardown. Named peers survive logger restarts.
inline HANDLE LogRingReadyEvent() {
    static const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, CE_SESSION_NAMESPACE L"LogRingReady_1");
    return event;
}

inline void NotifyLogConsumer() {
    if (const HANDLE event = LogRingReadyEvent())
        SetEvent(event);
}

}  // namespace ce::logging
