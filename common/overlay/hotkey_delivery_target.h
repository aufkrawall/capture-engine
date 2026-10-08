// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include <windows.h>

// Immutable route to an already-created thread message queue. The host chooses
// its private message; the keyboard hook never depends on controller globals.
class HotkeyDeliveryTarget {
public:
    constexpr HotkeyDeliveryTarget(DWORD thread = 0, UINT message = 0) noexcept : thread_(thread), message_(message) {}
    constexpr bool IsValid() const {
        return thread_ != 0 && message_ >= WM_APP && message_ <= 0xFFFF;
    }
    bool Post(int hotkeyId, int virtualKey) const {
        if (!IsValid()) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return false;
        }
        return PostThreadMessageW(thread_, message_, static_cast<WPARAM>(hotkeyId), static_cast<LPARAM>(virtualKey)) !=
               FALSE;
    }
    constexpr bool operator==(const HotkeyDeliveryTarget&) const = default;

private:
    DWORD thread_;
    UINT message_;
};
