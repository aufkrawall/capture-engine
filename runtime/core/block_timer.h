// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include "common/logging/logging.h"

#include <windows.h>

namespace ce::engine {

// Measures how long the thread that runs the runtime spends inside a blocking section, so a starved
// tray or hotkey path leaves a diagnostic (ported from the controller's MainThreadBlockTimer).
class BlockTimer {
public:
    explicit BlockTimer(const char* label) : label_(label), startMs_(GetTickCount64()) {}
    BlockTimer(const BlockTimer&) = delete;
    BlockTimer& operator=(const BlockTimer&) = delete;
    ~BlockTimer() {
        const ULONGLONG elapsedMs = GetTickCount64() - startMs_;
        if (elapsedMs >= 250) {
            LogWarn("[Controller] Main-thread blocked %llums in %s — tray + global hotkeys were unresponsive this long",
                    static_cast<unsigned long long>(elapsedMs), label_);
        }
    }

private:
    const char* label_;
    ULONGLONG startMs_;
};

}  // namespace ce::engine
