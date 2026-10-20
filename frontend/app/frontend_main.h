// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include <windows.h>

#include <string>

// What WinMain parsed for the frontend (the CaptureEngine app's own command line).
struct FrontendLaunch {
    std::string configPath;      // active-code-page path of config.ini
    std::string settingsPath;    // UTF-8, absolute: the --config= file; empty = the package's config.ini
    bool autoRecord = false;     // --auto-record=DELAY_MS,DURATION_MS (autonomous testing)
    DWORD autoRecordDelayMs = 3000;
    DWORD autoRecordDurationMs = 10000;
    std::string deferredLaunch;  // --launch <command> (active code page), run once the runtime is ready
};

inline void PrimeStartupCursor() {
    MSG msg = {};
    PeekMessage(&msg, NULL, 0, 0, PM_NOREMOVE);
    HCURSOR arrow = LoadCursor(nullptr, IDC_ARROW);
    if (arrow) {
        SetCursor(arrow);
    }
}

// The CaptureEngine app: tray, console handler and the app's automation, driving the runtime through its public C
// API (include/cengine/cengine.hpp). WinMain dispatches to it until the role host and the frontend become separate
// binaries (llm-wiki/library/05, M7).
int FrontendMain(HINSTANCE hInstance, const FrontendLaunch& launch);
