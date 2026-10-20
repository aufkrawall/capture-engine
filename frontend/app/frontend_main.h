// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include <windows.h>

#include <string>

namespace ce::runtime {
class RuntimePackagePaths;
}

// What WinMain parsed for the frontend (the CaptureEngine app's own command line).
struct FrontendLaunch {
    std::string configPath;      // active-code-page path of config.ini
    bool autoRecord = false;     // --auto-record=DELAY_MS,DURATION_MS (autonomous testing)
    DWORD autoRecordDelayMs = 3000;
    DWORD autoRecordDurationMs = 10000;
    std::string deferredLaunch;  // --launch <command> (active code page), run once the runtime is ready
};

// The CaptureEngine app: tray, console handler and the app's automation, driving the runtime engine. WinMain
// dispatches to it until the role host and the frontend become separate binaries (llm-wiki/library/05, M7).
int FrontendMain(HINSTANCE hInstance, const ce::runtime::RuntimePackagePaths& paths, const FrontendLaunch& launch);
