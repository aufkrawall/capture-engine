// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include <windows.h>

inline void PrimeStartupCursor() {
    MSG msg = {};
    PeekMessage(&msg, NULL, 0, 0, PM_NOREMOVE);
    HCURSOR arrow = LoadCursor(nullptr, IDC_ARROW);
    if (arrow) {
        SetCursor(arrow);
    }
}

// The CaptureEngine app: its command line, tray, console handler and automation, driving the runtime through its
// public C API (include/cengine/cengine.hpp). WinMain dispatches to it before any role prelude runs, so this process
// initializes nothing of the runtime's own; the runtime it creates owns settings, logging and the session claim
// (llm-wiki/library/05).
int FrontendMain(HINSTANCE hInstance);
